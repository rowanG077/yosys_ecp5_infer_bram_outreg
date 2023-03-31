# yosys-ecp5-infer-bram-outreg
Plugin for yosys that allows inference of output registers on the ECP5 DP16KD memory primitive.

# Compile:

- Make sure you have `"yosys-config"`` in the $PATH
- Run "make" in this directory

# Usage:

Run yosys with `"-m ecp5_infer_bram_outreg.so"`. Then use the `"ecp5_infer_bram_outreg"`
command after running synthesis for the ECP5 (`"synth_ecp5 ..."`).
