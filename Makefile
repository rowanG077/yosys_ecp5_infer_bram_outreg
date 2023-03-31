ecp5_infer_bram_outreg.so: $(PENELOPE_OBJS)
	$(shell yosys-config --cxx --cxxflags -O2 -o ecp5_infer_bram_outreg.o -shared ecp5_infer_bram_outreg.cc)

clean:
	rm ecp5_infer_bram_outreg.d ecp5_infer_bram_outreg.o

-include *.d
