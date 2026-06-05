import sys
import re
import os
import argparse
import subprocess
import time
import random
from names_generator import generate_name

def get_build_defaults(build_script_path):
    defaults = {'BRANCH': 'perceptron', 'L1I_PREFETCHER': 'no'}
    if os.path.exists(build_script_path):
        with open(build_script_path, 'r') as f:
            content = f.read()
            branch_match = re.search(r'^\s*BRANCH\s*=\s*([a-zA-Z0-9_-]+)', content, re.M)
            if branch_match:
                defaults['BRANCH'] = branch_match.group(1)
            l1i_match = re.search(r'^\s*L1I_PREFETCHER\s*=\s*([a-zA-Z0-9_-]+)', content, re.M)
            if l1i_match:
                defaults['L1I_PREFETCHER'] = l1i_match.group(1)
    return defaults

def get_random_tag():
    return generate_name()

def parse_files(exp_path, trace_path):
    with open(exp_path, 'r') as f1, open(trace_path, 'r') as f2:
        exp_content = f1.read()
        trace_content = f2.read()

    # 1. Parse Configurations & Variables
    vars_dict = dict(re.findall(r'^([A-Z0-9_]+)\s*=\s*(.*)$', exp_content, re.M))
    exps = re.findall(r'^([a-z][a-z0-9_]+)\s+(.*)$', exp_content, re.M)
    
    clean_exps = {}
    for name, args in exps:
        for var, val in vars_dict.items():
            args = args.replace(f"$({var})", val)
        clean_exps[name] = args
 
    # 2. Parse Traces
    traces = []
    blocks = trace_content.split('NAME=')[1:]
    for block in blocks:
        lines = block.strip().split('\n')
        t_info = {'NAME': lines[0].strip()}
        for l in lines:
            if '=' in l:
                k, v = l.split('=', 1)
                t_info[k.strip()] = v.strip()
        traces.append(t_info)

    # 3. Resolve Environment Variables and parse multiple traces
    final_exps = {}
    for key, val in clean_exps.items():
        step1 = re.sub(r'\$\((.*?)\)', lambda m: os.environ.get(m.group(1), m.group(0)), val)
        final_exps[key] = os.path.expandvars(step1)

    for t in traces:
        if 'TRACE' in t:
            raw_paths = t['TRACE'].split()
            resolved_paths = []
            for rpath in raw_paths:
                step1 = re.sub(r'\$\((.*?)\)', lambda m: os.environ.get(m.group(1), m.group(0)), rpath)
                resolved = os.path.normpath(os.path.expandvars(step1))
                resolved_paths.append(resolved)
            t['TRACES'] = resolved_paths
        else:
            t['TRACES'] = []
    
    return final_exps, traces

def main():
    parser = argparse.ArgumentParser(description="Hermes Multicore Experiment Runner")
    parser.add_argument("exp_cfg", help="Path to experiments.cfg")
    parser.add_argument("trace_cfg", help="Path to traces.cfg")
    parser.add_argument("binary_prefix", help="Prefix of binary in $HERMES_HOME/bin/ (e.g. glc-perceptron-no-multi-multi-multi-multi or glc-multi-multi-multi-multi)")
    parser.add_argument("warm", type=int, help="Warmup instructions")
    parser.add_argument("sim", type=int, help="Simulation instructions")
    parser.add_argument("-o", "--outdir", default="results_multicore", help="Directory to save logs")
    parser.add_argument("-j", "--jobs", type=int, default=4, help="Number of parallel slots (N)")
    
    args = parser.parse_args()

    # Get absolute paths and check directory structure
    hermes_home = os.environ.get('HERMES_HOME', os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
    
    if not os.path.exists(args.outdir):
        os.makedirs(args.outdir)

    configs, traces = parse_files(args.exp_cfg, args.trace_cfg)

    # 1. Infer required core counts and auto-compile binaries if missing
    required_cores = set()
    for t in traces:
        if t['TRACES']:
            required_cores.add(len(t['TRACES']))
        else:
            print(f"[ERROR] Trace config entry '{t.get('NAME', 'unknown')}' has no valid TRACE field.")
            sys.exit(1)

    cores_to_bin_path = {}
    for cores in sorted(required_cores):
        bin_dir = os.path.join(hermes_home, "bin")
        
        # Reconstruct the expected binary name prefix
        parts = args.binary_prefix.split('-')
        if len(parts) == 5:
            uarch, l1d, l2c, llc, repl = parts
            build_script = os.path.join(hermes_home, "build_champsim.sh")
            defaults = get_build_defaults(build_script)
            branch = defaults.get('BRANCH', 'perceptron')
            l1i = defaults.get('L1I_PREFETCHER', 'no')
            expected_prefix = f"{uarch}-{branch}-{l1i}-{l1d}-{l2c}-{llc}-{repl}"
        elif len(parts) == 7:
            expected_prefix = args.binary_prefix
        else:
            expected_prefix = args.binary_prefix
            
        bin_prefix = f"{expected_prefix}-{cores}core-"
        
        # Check if any tagged binary exists matching the prefix
        existing_bins = []
        if os.path.exists(bin_dir):
            existing_bins = [f for f in os.listdir(bin_dir) if f.startswith(bin_prefix)]
            
        if existing_bins:
            # Sort by modification time to get the newest one
            existing_bins.sort(key=lambda x: os.path.getmtime(os.path.join(bin_dir, x)), reverse=True)
            bin_name = existing_bins[0]
            bin_path = os.path.join(bin_dir, bin_name)
            print(f"[INFO] Found existing tagged binary: {bin_name}")
            cores_to_bin_path[cores] = bin_path
        else:
            print(f"[WARN] No binary starting with {bin_prefix} found. Building it now for {cores} cores...")
            build_script = os.path.join(hermes_home, "build_champsim.sh")
            if os.path.exists(build_script):
                # Parse build options from the prefix
                if len(parts) == 5:
                    uarch, l1d, l2c, llc, repl = parts
                elif len(parts) == 7:
                    uarch = parts[0]
                    l1d = parts[3]
                    l2c = parts[4]
                    llc = parts[5]
                    repl = parts[6]
                else:
                    uarch = parts[0] if len(parts) > 0 else "glc"
                    l1d = parts[1] if len(parts) > 1 else "multi"
                    l2c = parts[2] if len(parts) > 2 else "multi"
                    llc = parts[3] if len(parts) > 3 else "multi"
                    repl = parts[4] if len(parts) > 4 else "multi"
                
                dram_ch = "2" if int(cores) > 1 else "1"
                log_dram_ch = "1" if int(cores) > 1 else "0"
                
                build_cmd = [build_script, uarch, l1d, l2c, llc, repl, str(cores), dram_ch, log_dram_ch]
                print(f"[INFO] Executing: {' '.join(build_cmd)}")
                
                res = subprocess.run(build_cmd, cwd=hermes_home, capture_output=True, text=True)
                
                # Search for "Binary: bin/<name>" in build output
                match = re.search(r'Binary:\s*bin/(\S+)', res.stdout)
                if match:
                    compiled_bin_name = match.group(1)
                else:
                    compiled_bin_name = f"{expected_prefix}-{cores}core-{dram_ch}ch"
                    print(f"[WARN] Could not find binary name in build output. Defaulting to: {compiled_bin_name}")
                
                original_bin_path = os.path.join(bin_dir, compiled_bin_name)
                
                if res.returncode != 0 or not os.path.exists(original_bin_path):
                    print("--- STDOUT ---")
                    print(res.stdout)
                    print("--- STDERR ---")
                    print(res.stderr)
                    print(f"[ERROR] Failed to compile binary. retcode {res.returncode} and {original_bin_path} exists: {os.path.exists(original_bin_path)} ")
                    sys.exit(1)
                
                tag = get_random_tag()
                tagged_bin_name = f"{compiled_bin_name}-{tag}"
                bin_path = os.path.join(bin_dir, tagged_bin_name)
                os.rename(original_bin_path, bin_path)
                print(f"[SUCCESS] Compilation completed and binary renamed to {tagged_bin_name}")
                cores_to_bin_path[cores] = bin_path
            else:
                print(f"[ERROR] Build script {build_script} not found.")
                sys.exit(1)

    print(f"Total traces: {len(traces)} | configs: {len(configs)}")
    
    # Create config folders
    for c_name, c_flags in configs.items():
        absolute_dir = os.path.join(args.outdir, c_name)
        if not os.path.exists(absolute_dir):
            os.makedirs(absolute_dir)
    
    # Prepare all tasks
    task_queue = []
    runjobs_log_path = os.path.join(args.outdir, "runjobs.log")
    with open(runjobs_log_path, "w") as runjobs:
        for t in traces:
            cores = len(t['TRACES'])
            bin_path = cores_to_bin_path[cores]
            repeated_traces = " ".join(t['TRACES'])

            for c_name, c_flags in configs.items():
                absolute_dir = os.path.join(args.outdir, c_name)

                run_name = f"{t['NAME']}_{c_name}"
                log_file = os.path.join(absolute_dir, f"{run_name}.log")

                cmd = (
                    f"{bin_path} --warmup_instructions={args.warm} "
                    f"--simulation_instructions={args.sim} {c_flags} "
                    f"-o {run_name} -traces {repeated_traces}"
                )
                task_queue.append({'name': run_name, 'cmd': cmd, 'log': log_file})
                runjobs.write(cmd+'\n')

    active_procs = []
    print(f"Total tasks: {len(task_queue)} | Slots: {args.jobs}\n")

    finished_jobs = 0
    jobstatus_path = os.path.join(args.outdir, "jobstatus.log")
    with open(jobstatus_path, "w") as jobstatus:
        while task_queue or active_procs:
            # Fill available slots
            while len(active_procs) < args.jobs and task_queue:
                task = task_queue.pop(0)
                jobstatus.write(f"[START] {task['name']}\n")
                jobstatus.flush()
                
                f = open(task['log'], "w")
                p = subprocess.Popen(task['cmd'], shell=True, stdout=f, stderr=subprocess.STDOUT)
                active_procs.append({'process': p, 'name': task['name'], 'file': f, 'time': time.time()})

            # Check for finished processes
            for ap in active_procs[:]:
                if ap['process'].poll() is not None: # None means still running
                    ap['file'].close()
                    duration = time.time() - ap['time'] 
                    hours, rem = divmod(duration, 3600)
                    minutes, seconds = divmod(rem, 60)
                    duration_str = f"{int(hours)}h {int(minutes)}m {seconds:.2f}s"
                    
                    status = "SUCCESS" if ap['process'].returncode == 0 else "FAILED"

                    if status == "SUCCESS":
                        finished_jobs = finished_jobs + 1
                    
                    print(f"Done {finished_jobs}/{finished_jobs + len(task_queue) + len(active_procs) - 1}", end="\r", flush=True)

                    jobstatus.write(f"[DONE]  {ap['name']} (Status: {status}, TAT: {duration_str})\n")
                    jobstatus.flush()
                    active_procs.remove(ap)

            time.sleep(0.5) # Prevent high CPU usage by the manager loop

    print("\nAll experiments completed.")

if __name__ == "__main__":
    main()
