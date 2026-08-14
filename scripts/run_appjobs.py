import subprocess
import time
import sys
import re
import os
import argparse
import subprocess
import time
from pathlib import Path


def run_pair(pair_index, sim_cmd, pin_cmd):
    print("=" * 60)
    print(f"Starting Job Pair #{pair_index}")
    print(f"Time: {time.strftime('%Y-%m-%d %H:%M:%S')}")
    print("=" * 60)
    
    start_time = time.time()
    
    # Start both processes (using shell=True to handle output redirection like > /dev/null)
    sim_process = subprocess.Popen(sim_cmd, shell=True)
    pin_process = subprocess.Popen(pin_cmd, shell=True)
    
    sim_status = None
    pin_status = None
    
    # Monitor loop
    try:
        while True:
            sim_ret = sim_process.poll()
            pin_ret = pin_process.poll()
            
            # If both finished naturally
            if sim_ret is not None and pin_ret is not None:
                sim_status = sim_ret
                pin_status = pin_ret
                break
                
            # If Simulator finished/failed first, terminate PIN
            if sim_ret is not None and pin_ret is None:
                sim_status = sim_ret
                pin_status = "Killed (Partner exited first)"
                pin_process.terminate()
                pin_process.wait()
                break
                
            # If PIN finished/failed first, terminate Simulator
            if pin_ret is not None and sim_ret is None:
                pin_status = pin_ret
                sim_status = "Killed (Partner exited first)"
                sim_process.terminate()
                sim_process.wait()
                break
                
            time.sleep(0.5)
            
    except Exception as e:
        # Fallback safety cleanup
        sim_process.terminate()
        pin_process.terminate()
        sim_status = f"Error: {str(e)}"
        pin_status = f"Error: {str(e)}"

    end_time = time.time()
    duration = round(end_time - start_time, 2)
    
    print("-" * 60)
    print(f"Finished Job Pair #{pair_index}")
    print(f"Duration: {duration} seconds")
    print(f"ChampSim Status: {sim_status}")
    print(f"PIN Tracer Status: {pin_status}")
    print("=" * 60 + "\n")

def main(job_file):
    if not os.path.exists(job_file):
        print(f"Error: Job file '{job_file}' not found.")
        sys.exit(1)
        
    with open(job_file, 'r') as f:
        lines = [line.strip() for line in f if line.strip() and not line.startswith('#')]
        
    if len(lines) % 2 != 0:
        print("Error: Job file lines must be in pairs (ChampSim command followed by PIN command).")
        sys.exit(1)
        
    pair_index = 1
    for i in range(0, len(lines), 2):
        sim_cmd = lines[i]
        pin_cmd = lines[i+1]
        run_pair(pair_index, sim_cmd, pin_cmd)
        pair_index += 1

    print("All job pairs from file have completed execution.")

if __name__ == "__main__":
    # Pass 'jobs.txt' as the argument or change filename below
    parser = argparse.ArgumentParser(description="Hermes Experiment Runner")
    parser.add_argument("-j", "--jobs", help="Path to job file")
    job_filename = parser.parse_args().jobs or "jobs.txt"
    main(job_filename)