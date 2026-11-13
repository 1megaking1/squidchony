# Host tests for the firmware's portable code.
#   make -C tests/host        build and run everything
FW := ../../firmware/squidchrony
CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter

.PHONY: test clean
test: test_dsp test_app replay
	./test_app
	./test_dsp

test_dsp: test_dsp.cpp $(FW)/dsp.cpp $(FW)/dsp.h $(FW)/fir_coeffs.h
	$(CXX) $(CXXFLAGS) -DDSP_DEBUG -I$(FW) -o $@ test_dsp.cpp $(FW)/dsp.cpp

test_app: test_app.cpp $(FW)/settings.cpp $(FW)/settings.h $(FW)/shotlog.cpp $(FW)/shotlog.h $(FW)/dsp.h
	$(CXX) $(CXXFLAGS) -I$(FW) -o $@ test_app.cpp $(FW)/settings.cpp $(FW)/shotlog.cpp

# Replays a recorded shot (the phone's "Last shot (WAV)") through the firmware's processing.
replay: replay.cpp $(FW)/dsp.cpp $(FW)/dsp.h $(FW)/settings.cpp $(FW)/settings.h
	$(CXX) $(CXXFLAGS) -I$(FW) -o $@ replay.cpp $(FW)/dsp.cpp $(FW)/settings.cpp

clean:
	rm -f test_dsp test_app replay
