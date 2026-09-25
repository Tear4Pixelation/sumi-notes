CXX ?= g++
CXXFLAGS ?= -O2 -g -std=c++17 -Wall -Wextra
# the recognizer is the one built into Write, not a copy
APPDIR = ../../syncscribble
CXXFLAGS += -I. -I$(APPDIR)

SOURCES = $(APPDIR)/shaperec.cpp synth.cpp strokefile.cpp evaluate.cpp
HEADERS = geom.h $(APPDIR)/shaperecgeom.h $(APPDIR)/shaperec.h synth.h strokefile.h

evaluate: $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ $(SOURCES)

test: evaluate
	./evaluate

clean:
	rm -f evaluate report.html

.PHONY: test clean
