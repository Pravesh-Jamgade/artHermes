import sys
import re
import os
import argparse
import subprocess
import time

def parse_files(exp_path, trace_path):
    with open(exp_path, 'r') as f1, open(trace_path, 'r') as f2:
        exp_content = f1.read()
        trace_content = f2.read()

    # 1. Parse Configurations & Variables
    vars_dict = dict(re.findall(r'^([A-Z0-9_]+)\s*=\s*(.*)$', exp_content, re.M))
    exps = re.findall(r'^([a-z][a-z0-9_]+)\s+(.*)$', exp_content, re.M)
    
    print(vars_dict)
    clean_exps = {}
    for name, args in exps:
        for var, val in vars_dict.items():
            args = args.replace(f"$({var})", val)
        clean_exps[name] = args
 
    # 2. Parse Traces
    traces = []
    blocks = trace_content.split('APP_PATH=')[1:]
    for block in blocks:
        lines = block.strip().split('\n')
        t_info = {'APP_PATH': lines[0].strip()}
        for l in lines:
            if '=' in l:
                k, v = l.split('=', 1)
                t_info[k.strip()] = v.strip()
        traces.append(t_info)

    # 3. Resolve Environment Variables
    final_exps = {}
    for key, val in clean_exps.items():
        step1 = re.sub(r'\$\((.*?)\)', lambda m: os.environ.get(m.group(1), m.group(0)), val)
        final_exps[key] = os.path.expandvars(step1)

    for t in traces:
        if 'APP_PATH' in t:
            step1 = re.sub(r'\$\((.*?)\)', lambda m: os.environ.get(m.group(1), m.group(0)), t['APP_PATH'])
            t['APP_PATH'] = os.path.normpath(os.path.expandvars(step1))    
        if 'APP_INPUT' in t:
            step1 = re.sub(r'\$\((.*?)\)', lambda m: os.environ.get(m.group(1), m.group(0)), t['APP_INPUT'])
            t['APP_INPUT'] = os.path.normpath(os.path.expandvars(step1))    
    
    print(traces)
    exit(0)
    return final_exps, traces

def main():
    parser = argparse.ArgumentParser(description="Hermes Experiment Runner")
    parser.add_argument("exp_cfg", help="Path to experiments.cfg")
    parser.add_argument("trace_cfg", help="Path to traces.cfg")
    parser.add_argument("binary", help="Name of the binary in $HERMES_HOME/bin/")
    parser.add_argument("warm", type=int, help="Warmup instructions")
    parser.add_argument("sim", type=int, help="Simulation instructions")
    parser.add_argument("-o", "--outdir", default="results", help="Directory to save logs")
    parser.add_argument("-j", "--jobs", type=int, default=4, help="Number of parallel slots (N)")
    parser.add_argument("-a", "--app-diven", type=int, default=0, help="App driven simulation")
    
    args = parser.parse_args()

    if not os.path.exists(args.outdir):
        os.makedirs(args.outdir)

    configs, traces = parse_files(args.exp_cfg, args.trace_cfg)

    print(f"Total traces: {len(traces)} | configs: {len(configs)}")
    hermes_home = os.environ.get('HERMES_HOME', '.')
    for c_name, c_flags in configs.items():
        rel_dir = os.path.join(args.outdir, c_name)
        absolute_dir = os.path.join(hermes_home, rel_dir)
        print(f"mkdir {absolute_dir}")
        if not os.path.exists(absolute_dir):
            os.makedirs(absolute_dir)
    
    # Prepare all tasks
    task_queue = []
    with open(os.path.join(args.outdir, "runjobs.log"), "w") as runjobs:
        for t in traces:
            for c_name, c_flags in configs.items():

                rel_dir = os.path.join(args.outdir, c_name)
                absolute_dir = os.path.join(hermes_home, rel_dir)

                run_name = f"{t['NAME']}_{c_name}"
                log_file = os.path.join(absolute_dir, f"{run_name}.log")
                bin_path = os.path.join(hermes_home, "bin", args.binary)

                cmd = (
                    f"{bin_path} --warmup_instructions={args.warm} "
                    f"--simulation_instructions={args.sim} {c_flags} "
                    f"-o {run_name} -traces {t['TRACE']}"
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
                        finished_jobs=finished_jobs+1
                    print(f"Done {finished_jobs}/{len(task_queue)}", end="\r", flush=True)

                    jobstatus.write(f"[DONE]  {ap['name']} (Status: {status}, TAT: {duration_str})\n")
                    jobstatus.flush()
                    active_procs.remove(ap)

            time.sleep(0.5) # Prevent high CPU usage by the manager loop

    print("\nAll experiments completed.")

if __name__ == "__main__":
    main()
