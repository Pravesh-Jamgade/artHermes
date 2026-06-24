#!/bin/bash

# Simple Multicore Simulation Runner for artHermes (ChampSim)
# This script compiles and runs a multi-core ChampSim simulation
# where each core executes a distinct trace file.

# Color definitions
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Source setvars.sh if available to get HERMES_HOME and HERMES_TRACE
if [ -f "./setvars.sh" ]; then
    log_info "Sourcing setvars.sh..."
    source ./setvars.sh
fi

if [ -z "$HERMES_HOME" ]; then
    HERMES_HOME=$(pwd)
    log_warn "HERMES_HOME not set. Using current directory: $HERMES_HOME"
fi

# Default parameters
CORES=4
WARMUP=10000000
SIMULATION=50000000
CONFIG_FILE=""
EXTRA_ARGS=""
TRACES=()

print_usage() {
    echo "Usage: $0 [options] -t <trace1> <trace2> ..."
    echo "Options:"
    echo "  -c, --cores <num>       Number of CPU cores (default: 4)"
    echo "  -w, --warmup <num>      Warmup instructions (default: 10000000)"
    echo "  -s, --sim <num>         Simulation instructions (default: 50000000)"
    echo "  -k, --config <file>     Path to configuration INI file (e.g. config/ocp_hermes.ini)"
    echo "  -e, --extra <args>      Extra command-line arguments to pass to ChampSim"
    echo "  -t, --traces <files>    List of trace files (space separated, one per core)"
    echo "  -h, --help              Show this help message"
}

# Parse command line options
while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--cores)
            CORES="$2"
            shift 2
            ;;
        -w|--warmup)
            WARMUP="$2"
            shift 2
            ;;
        -s|--sim)
            SIMULATION="$2"
            shift 2
            ;;
        -k|--config)
            CONFIG_FILE="$2"
            shift 2
            ;;
        -e|--extra)
            EXTRA_ARGS="$2"
            shift 2
            ;;
        -t|--traces)
            shift
            while [[ $# -gt 0 && ! "$1" =~ ^- ]]; do
                TRACES+=("$1")
                shift
            done
            ;;
        -h|--help)
            print_usage
            exit 0
            ;;
        *)
            log_error "Unknown option: $1"
            print_usage
            exit 1
            ;;
    esac
done

# Check if traces are provided
if [ ${#TRACES[@]} -eq 0 ]; then
    log_error "No trace files provided. Use the -t/--traces option."
    print_usage
    exit 1
fi

# Validate that trace count matches core count
if [ ${#TRACES[@]} -ne "$CORES" ]; then
    log_error "Number of trace files (${#TRACES[@]}) must match the number of cores ($CORES)."
    exit 1
fi

# Validate trace files
RESOLVED_TRACES=()
for trace in "${TRACES[@]}"; do
    if [ -f "$trace" ]; then
        RESOLVED_TRACES+=("$trace")
    elif [ -n "$HERMES_TRACE" ] && [ -f "$HERMES_TRACE/$trace" ]; then
        RESOLVED_TRACES+=("$HERMES_TRACE/$trace")
    elif [ -n "$HERMES_TRACE" ] && [ -f "$HERMES_TRACE/$(basename "$trace")" ]; then
        RESOLVED_TRACES+=("$HERMES_TRACE/$(basename "$trace")")
    else
        log_error "Trace file not found: $trace (also checked in $HERMES_TRACE)"
        exit 1
    fi
done

# Binary name convention used by build_champsim.sh
BINARY_NAME="glc-perceptron-no-multi-multi-multi-multi-${CORES}core-1ch"
BINARY_PATH="$HERMES_HOME/bin/$BINARY_NAME"

# Build the binary if it does not exist
if [ ! -f "$BINARY_PATH" ]; then
    log_warn "Binary $BINARY_PATH not found. Building it now..."
    if [ -f "./build_champsim.sh" ]; then
        ./build_champsim.sh glc multi multi multi multi "$CORES" 1 0
        if [ $? -ne 0 ] || [ ! -f "$BINARY_PATH" ]; then
            log_error "Failed to build ChampSim binary for $CORES cores."
            exit 1
        fi
        log_success "ChampSim binary compiled successfully: $BINARY_PATH"
    else
        log_error "Could not find build_champsim.sh in the current directory."
        exit 1
    fi
fi

# Build ChampSim command line
CMD="$BINARY_PATH --warmup_instructions=$WARMUP --simulation_instructions=$SIMULATION"

if [ -n "$CONFIG_FILE" ]; then
    if [ -f "$CONFIG_FILE" ]; then
        CMD="$CMD --config=$CONFIG_FILE"
    else
        log_error "Config file $CONFIG_FILE not found."
        exit 1
    fi
fi

if [ -n "$EXTRA_ARGS" ]; then
    CMD="$CMD $EXTRA_ARGS"
fi

# Append traces
CMD="$CMD -traces ${RESOLVED_TRACES[*]}"

log_info "Running multicore simulation..."
log_info "Command: $CMD"

# Create output log directory if it doesn't exist
mkdir -p "$HERMES_HOME/multicore_logs"
LOG_FILE="$HERMES_HOME/multicore_logs/multicore_${CORES}c_$(date +%Y%m%d_%H%M%S).log"
log_info "Simulation output will be logged to $LOG_FILE"

# Execute command
$CMD 2>&1 | tee "$LOG_FILE"

if [ ${PIPESTATUS[0]} -eq 0 ]; then
    log_success "Simulation completed successfully."
else
    log_error "Simulation failed. Check the log file: $LOG_FILE"
    exit 1
fi
