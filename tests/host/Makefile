# Host tests for the firmware's portable signal-processing code.
#   make -C tests/host        build and run
FW := ../../firmware/squidchrony
CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter

.PHONY: test clean
test: test_dsp
	./test_dsp

test_dsp: test_dsp.cpp $(FW)/dsp.cpp $(FW)/dsp.h $(FW)/fir_coeffs.h
	$(CXX) $(CXXFLAGS) -DDSP_DEBUG -I$(FW) -o $@ test_dsp.cpp $(FW)/dsp.cpp

clean:
	rm -f test_dsp
