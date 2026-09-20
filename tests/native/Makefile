# Native (PC) unit tests for RemoteConfigCore.h: `make -C tests/native test`
# ArduinoJson: set ARDUINOJSON to its `src` directory (defaults to the copy PlatformIO downloaded, else the system one).
ARDUINOJSON ?= $(firstword $(wildcard ../../.pio/libdeps/*/ArduinoJson/src) /usr/include)
CXX ?= g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wno-unused-function -O0 -g -DFIREWALL_MODE -DREMOTE_CONFIG \
           -I stubs -I ../.. -I $(ARDUINOJSON)

test: test_remote_config
	./test_remote_config

test_remote_config: test_remote_config.cpp ../../RemoteConfigCore.h ../../FirewallMode.h
	$(CXX) $(CXXFLAGS) -o $@ test_remote_config.cpp

clean:
	rm -f test_remote_config
.PHONY: test clean
