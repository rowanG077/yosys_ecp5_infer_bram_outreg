/*
 *  Copyright (C) 2023 Rowan Goemans <goemansrowan@gmail.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include <cstddef>
#include <vector>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct Ecp5InferBramOutRegBRAMPass : public Pass {
	Ecp5InferBramOutRegBRAMPass() : Pass("ecp5_infer_bram_outreg", "ECP5: Infer output register for bram cells") { }
	void help() override
	{
		log("\n");
		log("    ecp5_infer_bram_outreg\n");
		log("\n");
		log("Pack registers found at the output of DP16KD block ram cells, run it after ran `synth_ecp5`.\n");
		log("\n");
	}

	bool ff_is_mergeable(
		SigMap &sigmap,
		RTLIL::Cell *dp16kd,
		const RTLIL::IdString dp16kd_port_id,
		const RTLIL::IdString dp16kd_id_clk,
		const RTLIL::IdString do16kd_id_rst,
		const RTLIL::IdString dp16kd_id_clkmux,
		const RTLIL::SigSpec required_en,
		RTLIL::Cell *trellis_ff
	)
	{
		if (dp16kd->getParam(dp16kd_id_clkmux).decode_string() != log_id(dp16kd_id_clk)) {
			log("\tDP16KD CLKMUX setting indicates clock is unused.\n");
			return false;
		}

		std::string trellis_log = "TRELLIS_FF at DP16KD." + std::string(log_id(dp16kd_port_id));

		if (trellis_ff->getParam(ID(CLKMUX)).decode_string() != "CLK") {
			log("\t%s CLKMUX setting indicates clock is unused.\n", trellis_log.c_str());
			return false;
		}

		auto dp16kd_clk = sigmap(dp16kd->getPort(dp16kd_id_clk)[0]);
		auto dp16kd_rst = sigmap(dp16kd->getPort(do16kd_id_rst)[0]);

		auto ff_clk = sigmap(trellis_ff->getPort(ID(CLK))[0]);
		if (dp16kd_clk != ff_clk) {
			log("\t%s Clock uses a different clock then the DP16KD.\n", trellis_log.c_str());
			return false;
		}

		// TRELLIS_FF does not always have a LSR port??? Could not find this in the wild
		auto ff_has_rst = trellis_ff->getParam(ID(LSRMUX)).decode_string() == "LSR";
		if (ff_has_rst && sigmap(trellis_ff->getPort(ID(LSR))[0]) != dp16kd_rst) {
			log("\t%s LSR uses a different clock then the DP16KD.\n", trellis_log.c_str());
			return false;
		}

		if (!ff_has_rst && dp16kd_rst != SigBit(false)) {
			log("\t%s doesn't have a LSR while DP16KD LSR is not always off.\n", trellis_log.c_str());
			return false;
		}

		// TRELLIS_FF does not always have a CE port
		auto ff_has_en = trellis_ff->getParam(ID(CEMUX)).decode_string() == "CE";
		if (ff_has_en && sigmap(trellis_ff->getPort(ID(CE))) != required_en) {
			log("\t%s uses a different enable the prior TRELLIS_FFs.\n", trellis_log.c_str());
			return false;
		}

		if (!ff_has_en && required_en != SigBit(true)) {
			log("\t%s has no enable while the prior TRELLIS_FFs do.\n", trellis_log.c_str());
			return false;
		}

		auto dp16kd_gsr = dp16kd->getParam(ID(GSR)).decode_string() == "ENABLED";
		auto ff_gsr = trellis_ff->getParam(ID(GSR)).decode_string() == "ENABLED";

		if (dp16kd_gsr != ff_gsr) {
			log("\t%s GSR does not match the TRELLIS_FF.\n", trellis_log.c_str());
			return false;
		}

		auto dp16kd_async_reset = dp16kd->getParam(ID(RESETMODE)).decode_string() == "ASYNC";
		auto ff_async_reset = trellis_ff->getParam(ID(SRMODE)).decode_string() == "ASYNC";

		// Differing reset configs only matter if the resets are actually used
		auto static_reset = dp16kd_rst == SigBit(true) || dp16kd_rst == SigBit(false);
		if (static_reset == false && dp16kd_async_reset != ff_async_reset) {
			log(
				"\t%s uses %s resets while the DP16KD uses %s resets.\n",
				trellis_log.c_str(),
				ff_async_reset ? "ASYNC" : "SYNC",
				dp16kd_async_reset ? "ASYNC" : "SYNC"
			);
			return false;
		}

		return true;
	}

	void pack_ff_into_dp16kd(
		Module *module,
		dict<SigBit, std::vector<std::pair<Cell*, RTLIL::IdString>>> &receivers,
		std::vector<std::pair<RTLIL::IdString, RTLIL::SigSpec>> &datout_ports,
		RTLIL::Cell *dp16kd,
		const RTLIL::IdString id_oce,
		const RTLIL::IdString id_ocemux,
		RTLIL::SigSpec& oce,
		const RTLIL::IdString id_regmode
	)
	{
		// Disconnect the TRELLIS_FF, wire the output of the DP16KD directly
		// to the output SigSpec of the TRELLIS_FF.
		// Finally set the DP16KD to use an output register and wire up oce
		for (auto port : datout_ports) {
			auto& id = port.first;
			auto& dout = port.second;

			auto recv_cnt = receivers.count(dout);

			if (recv_cnt == 0)
				continue;
			
			auto& receiver = receivers[dout][0];
			auto receiver_cell = receiver.first;
			auto receiver_port = receiver.second;

			log("\tDisconnecting %s at DP16KD.%s.\n", log_id(receiver_cell->name), log_id(id));

			auto q = receiver_cell->getPort(ID(Q))[0];
			receiver_cell->unsetPort(ID(DI));
			receiver_cell->unsetPort(ID(Q));
			module->connect(dout, q);
		}

		log("\tSetting %s to removed TRELLIS_FF enable.\n", log_id(id_oce));
		dp16kd->unsetPort(id_oce);
		dp16kd->setPort(id_oce, oce);
		log("\tSetting %s to %s.\n", log_id(id_ocemux), log_id(id_oce));
		dp16kd->setParam(id_ocemux, Const(log_id(id_oce)));

		log("\tSetting %s to %s.\n", log_id(id_regmode), "OUTREG");
		dp16kd->setParam(id_regmode, Const("OUTREG"));
	}

	void try_pack_port(
		Module *module,
		dict<SigBit, std::vector<std::pair<Cell*, RTLIL::IdString>>> &receivers,
		SigMap &sigmap,
		RTLIL::Cell *dp16kd,
		const RTLIL::IdString id_clk,
		const RTLIL::IdString id_rst,
		const RTLIL::IdString id_oce,
		const RTLIL::IdString id_ocemux,
		const RTLIL::IdString id_clkmux,
		const RTLIL::IdString id_regmode,
		const std::vector<RTLIL::IdString> &datout_ids
	)
	{
		std::vector<std::pair<RTLIL::IdString, RTLIL::SigSpec>> datout_ports;
		datout_ports.reserve(datout_ids.size());

		for (int i = 0; i < int(datout_ids.size()); ++i) {
			auto& id = datout_ids[i];
			if (dp16kd->hasPort(id)) {
				datout_ports.emplace_back(std::make_pair(id, sigmap(dp16kd->getPort(id))));
			}
		}

		bool at_least_one_ff = false;
		RTLIL::SigSpec first_en = SigSpec(true);
		for (auto& port : datout_ports) {
			auto& id = port.first;
			auto& dout = port.second;

			auto recv_cnt = receivers.count(dout);
			if (recv_cnt > 1) {
				log("\tDP16KD.%s drives multiple inputs.", log_id(id));
				return;
			}
			else if (recv_cnt == 0) {
				continue;
			}

			auto& receiver = receivers[dout][0];
			auto receiver_cell = receiver.first;
			auto receiver_port = receiver.second;

			// This detection does NOT work. Not sure why
			if (receiver_cell == nullptr) {
				log("\tDP16KD.%s is connected to a module port.\n", log_id(id));
				return;
			}

			if (receiver_cell->type != ID(TRELLIS_FF) || receiver_port != ID(DI)) {
				log(
					"\tDP16KD.%s is not connected to a TRELLIS_FF DI but to a %s %s\n",
					log_id(id),
					log_id(receiver_cell->type),
					log_id(receiver_port)
				);
				return;
			}
			
			auto ff_has_en = receiver_cell->getParam(ID(CEMUX)).decode_string() == "CE";
			if (ff_has_en == true && at_least_one_ff == false) {
				first_en = sigmap(receiver_cell->getPort(ID(CE)));
			}

			if (!ff_is_mergeable(sigmap, dp16kd, id, id_clk, id_rst, id_clkmux, first_en, receiver_cell))
				return;

			at_least_one_ff = true;
		}

		if (!at_least_one_ff) {
			log("\tNo dp16kd data ports are connected to a TRELLIS_FF\n");
			return;
		}

		pack_ff_into_dp16kd(module, receivers, datout_ports, dp16kd, id_oce, id_ocemux, first_en, id_regmode);
	}

	void try_pack_outreg_dp16dk(
		Module *module,
		dict<SigBit, std::vector<std::pair<Cell*, RTLIL::IdString>>> &receivers,
		SigMap &sigmap,
		RTLIL::Cell *dp16kd
	) {
		static const std::vector<RTLIL::IdString> ID_DOA = {
			ID(DOA0), ID(DOA1), ID(DOA2), ID(DOA3), ID(DOA4), ID(DOA5),
			ID(DOA6), ID(DOA7), ID(DOA8), ID(DOA9), ID(DOA10),ID(DOA11),
			ID(DOA12),ID(DOA13),ID(DOA14),ID(DOA15),ID(DOA16),ID(DOA17),
			ID(DOA18),ID(DOA19),ID(DOA20),ID(DOA21),ID(DOA22),ID(DOA23),
			ID(DOA24),ID(DOA25),ID(DOA26),ID(DOA27),ID(DOA28),ID(DOA29),
			ID(DOA30),ID(DOA31),ID(DOA32),ID(DOA33),ID(DOA34),ID(DOA35)
		};

		static const std::vector<RTLIL::IdString> ID_DOB = {
			ID(DOB0), ID(DOB1), ID(DOB2), ID(DOB3), ID(DOB4), ID(DOB5),
			ID(DOB6), ID(DOB7), ID(DOB8), ID(DOB9), ID(DOB10),ID(DOB11),
			ID(DOB12),ID(DOB13),ID(DOB14),ID(DOB15),ID(DOB16),ID(DOB17),
			ID(DOB18),ID(DOB19),ID(DOB20),ID(DOB21),ID(DOB22),ID(DOB23),
			ID(DOB24),ID(DOB25),ID(DOB26),ID(DOB27),ID(DOB28),ID(DOB29),
			ID(DOB30),ID(DOB31),ID(DOB32),ID(DOB33),ID(DOB34),ID(DOB35)
		};

		int data_width_a = dp16kd->getParam(ID(DATA_WIDTH_A)).as_int();
		int data_width_b = dp16kd->getParam(ID(DATA_WIDTH_B)).as_int();

		bool output_reg_a = dp16kd->getParam(ID(REGMODE_A)).decode_string() == "OUTREG";
		bool output_reg_b = dp16kd->getParam(ID(REGMODE_B)).decode_string() == "OUTREG";

		log("Found DP16KD cell\n");

		log("Analyzing Port A\n");
		log("\tDATA_WIDTH %d, OUTREG enabled: %d\n", data_width_a, output_reg_a);

		if (output_reg_a == false) {
			try_pack_port(
				module, receivers, sigmap, dp16kd, ID(CLKA), ID(RSTA),
				ID(OCEA), ID(OCEAMUX), ID(CLKAMUX), ID(REGMODE_A), ID_DOA
			);
		}
		else {
			log("\tHas OUTREG already. Skipping...\n");
		}

		log("Analyzing Port B\n");
		log("\tDATA_WIDTH %d, OUTREG enabled: %d\n", data_width_b, output_reg_b);

		if (output_reg_b == false) {
			try_pack_port(
				module, receivers, sigmap, dp16kd, ID(CLKB), ID(RSTB),
				ID(OCEB), ID(OCEBMUX), ID(CLKBMUX), ID(REGMODE_B), ID_DOB
			);
		}
		else {
			log("\tHas OUTREG already. Skipping...\n");
		}

	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing ECP5_INFER_BRAM_OUTREG pass.\n");

		extra_args(args, 1, design, false);

		for (auto module : design->selected_modules())
		{
			log("Handling ECP5_INFER_BRAM_OUTREG in %s.\n", log_id(module));

			// dict containing a mapping from sigbits to the receivers
			// Used to check for usage of outputs from DP16KD ports
			dict<SigBit, std::vector<std::pair<Cell*, RTLIL::IdString>>> receivers;
			SigMap sigmap(module);
			
			// Intra-module usage
			for (auto cell : module->cells()) {
				for (auto &conn : cell->connections()) {
					if (cell->input(conn.first)) {
						for (auto b : sigmap(conn.second)) {
							receivers[b].emplace_back(std::make_pair(cell, conn.first));
						}
					}
				}
			}

			// Do not support inter-module
			for (auto wire : module->wires()) {
				if (wire->port_input) {
					log("wire %s is input\n", log_id(wire->name));
					for (auto b : sigmap(wire)) {
						receivers[b].emplace_back(std::make_pair(nullptr, wire->name));
					}
				}
				if (wire->port_output) {
					log("wire %s is output\n", log_id(wire->name));
					for (auto b : sigmap(wire)) {
						receivers[b].emplace_back(std::make_pair(nullptr, wire->name));
					}
				}
			}

			for (auto cell : module->selected_cells())
			{
				if (cell->type == ID(DP16KD))
				 	try_pack_outreg_dp16dk(module, receivers, sigmap, cell);
			}
		}

		Pass::call(design, "clean");
	}
} Ecp5InferBramOutRegBRAMPass;

PRIVATE_NAMESPACE_END
