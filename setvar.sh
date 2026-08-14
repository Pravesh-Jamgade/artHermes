#!/bin/bash

export HERMES_HOME=`pwd`
export PERL5LIB=$PERL5LIB:$HERMES_HOME/scripts
export HERMES_TRACE=/mnt/usb-Samsung_PSSD_T9_S743NS0X301609D-0\:0-part1/pravesh/
# export HERMES_TRACE=/media/pravesh/Storage/ALL_TRACES

# testing
export APP1=/media/pravesh/Storage/code/sims/artHermes/tracer/example/
export INPUT1=

# GAPBS bench
export GRAPH_APP=/home/pravesh/gapbs/
export GRAPH_INPUT=/mnt/usb-Samsung_PSSD_T9_S743NS0X301609D-0\:0-part1/upasna/gapbs/benchmark/graphs/
export GRAPH_PHASE_DIR=$HERMES_HOME/tracer/hot_phases