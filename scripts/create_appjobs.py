import sys
import re
import os
import argparse
import subprocess
import time
from pathlib import Path

def parse_files(exp_path, trace_path):
    with open(exp_path, 'r') as f1, open(trace_path, 'r') as f2:
        exp_content = f1.read()
        trace_content = f2.read()

    # 1. Parse Configurations & Variables
    vars_dict = dict(re.findall(r'^([A-Z0-9_]+)\s*=\s*(.*)$', exp_content, re.M))
    exps = re.findall(r'^([a-z][a-z0-9_]+)\s+(.*)$', exp_content, re.M)
    
    # print(vars_dict)
    clean_exps = {}
    for name, args in exps:
        for var, val in vars_dict.items():
            args = args.replace(f"$({var})", val)
        clean_exps[name] = args
 
    # 2. Parse Traces
    traces = []
    if 'APP_PATH=' in trace_content:
        blocks = trace_content.split('APP_PATH=')[1:]
        for block in blocks:
            lines = block.strip().split('\n')
            t_info = {'APP_PATH': lines[0].strip()}
            for l in lines:
                if '=' in l:
                    k, v = l.split('=', 1)
                    t_info[k.strip()] = v.strip()
            traces.append(t_info)
    else:
        blocks = trace_content.split('NAME=')[1:]
        for block in blocks:
            lines = block.strip().split('\n')
            t_info = {'NAME': lines[0].strip()}
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
        for key in ['APP_PATH', 'APP_INPUT', 'TRACE']:
            if key in t:
                step1 = re.sub(r'\$\((.*?)\)', lambda m: os.environ.get(m.group(1), m.group(0)), t[key])
                t[key] = os.path.normpath(os.path.expandvars(step1))

        ##### Creating new var: APP_TAG an APP_NAME 

        # Derive APP_NAME if not present
        if 'APP_NAME' not in t and 'APP_PATH' in t:
            t['APP_NAME'] = os.path.basename(t['APP_PATH'])
            
        # Derive APP_TAG if not present
        if 'APP_TAG' not in t:
            if 'APP_NAME' in t:
                if 'APP_INPUT' in t and t['APP_INPUT']:
                    t['APP_TAG'] = f"{t['APP_NAME']}_{os.path.basename(t['APP_INPUT'])}"
                else:
                    t['APP_TAG'] = t['APP_NAME']
            else:
                t['APP_TAG'] = "unknown_app"
                           
    return final_exps, traces

def path_tag(file_path: Path) -> str: 
    return "_".join(file_path.parent.parts)

def main():
    parser = argparse.ArgumentParser(description="Hermes Experiment Runner")
    parser.add_argument("exp_cfg", help="Path to experiments.cfg")
    parser.add_argument("trace_cfg", help="Path to traces.cfg")
    parser.add_argument("binary", help="Name of the binary in $HERMES_HOME/bin/")
    parser.add_argument("warm", type=int, help="Warmup instructions")
    parser.add_argument("sim", type=int, help="Simulation instructions")
    parser.add_argument("-o", "--outdir", default="results", help="Directory to save logs")
    parser.add_argument("-j", "--jobs", type=int, default=4, help="Number of parallel slots (N)")

    parser.add_argument("-a", "--app-driven", type=int, default=0, help="App driven simulation")
    # THIS: To enable this change pintool to allow all windows to be traced. In simulator then i can do 
    # something as simple as Skip 50M in each 100M window by setting knob:partial_window_trace option
    parser.add_argument("-g", "--greedy-tracing", type=int, default=0, help="Greedy tracing")
    # OR THIS
    parser.add_argument("-w", "--trace-windows", type=int, default=0, help="trace n hot windows")
    parser.add_argument("--hot-phases", type=str, default=0, help="hot phase file")
    
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
        
        # for each exp create a folder with for storing app-driven fifo buffer
        if args.app_driven:
            app_driven_dir = os.path.join(absolute_dir, "temp")
            if not os.path.exists(app_driven_dir):
                os.makedirs(app_driven_dir)
    
    # Prepare all tasks
    task_queue = []
    with open(os.path.join(args.outdir, "runjobs.log"), "w") as runjobs:
        for t in traces:
            for c_name, c_flags in configs.items():

                rel_dir = os.path.join(args.outdir, c_name)
                absolute_dir = os.path.join(hermes_home, rel_dir)
                fifo_dir = os.path.join(absolute_dir, "temp") if args.app_driven else None

                fifo_path = ""
                if args.app_driven and fifo_dir:
                    # Create a unique FIFO name for this trace and config
                    fifo_name = f"{t['APP_TAG']}.fifo"
                    fifo_path = os.path.join(fifo_dir, fifo_name)
                    
                    # Create the FIFO if it doesn't exist
                    if not os.path.exists(fifo_path):
                        os.mkfifo(fifo_path)
                    t['APP_FIFO_PATH'] = fifo_path

                trace_arg = t['APP_FIFO_PATH'] if args.app_driven else t.get('TRACE', '')

                run_name = f'{t['APP_TAG']}_{c_name}' if args.app_driven else f"{t['NAME']}_{c_name}"
                log_file = os.path.join(absolute_dir, f"{run_name}.log")
                sim_path = os.path.join(hermes_home, "bin", args.binary)
                pin_home = os.environ.get('PIN_ROOT', '.')
                pin_path = os.path.join(pin_home, "pin")
                pintool_path = os.path.join(hermes_home, "tracer/obj-intel64/app_hot_tracer.so")
                
                simcmd = (
                    f"{sim_path} --warmup_instructions={args.warm} "
                    f"--simulation_instructions={args.sim} {c_flags} "
                    f"-o {run_name} -traces {trace_arg}"
                )

                pincmd = None
                if args.app_driven:
                    pincmd = (
                        f"{pin_path} "
                        f"-t {pintool_path} -s 0 -t {args.sim} "
                        f"-w {args.trace_windows} -phase_file {args.hot_phases} -o {trace_arg} "
                        f"-- {t['APP_PATH']} {t['APP_INPUT']}"
                    )
                task_queue.append({'name': run_name, 'simcmd': simcmd, 'pincmd': pincmd, 'log': log_file})
                runjobs.write(simcmd+'\n')
                runjobs.write(pincmd+'\n' if pincmd else '')

    active_tasks = []
    total_tasks = len(task_queue)
    print(f"Total tasks: {total_tasks}  | Slots: {args.jobs}\n")

    # exit(0)

    def get_cpu_usage(tasks):
        usage = 0
        for at in tasks:
            for p_info in at['processes']:
                if p_info['proc'].poll() is None:
                    usage += 1
        return usage

    finished_jobs = 0
    jobstatus_path = os.path.join(args.outdir, "jobstatus.log")
    with open(jobstatus_path, "w") as jobstatus:
        while task_queue or active_tasks:
            # Check for finished processes / tasks first to free resources
            for at in active_tasks[:]:
                running_procs = []
                finished_procs = []
                for p_info in at['processes']:
                    poll_status = p_info['proc'].poll()
                    if poll_status is None:
                        running_procs.append(p_info)
                    else:
                        finished_procs.append((p_info, poll_status))
                
                # If some processes ended but others are still running, terminate the running ones
                if finished_procs and running_procs and not at.get('killed', False):
                    killed_any = False
                    for r_info in running_procs:
                        for f_info, _ in finished_procs:
                            if {f_info['type'], r_info['type']} == {'pin', 'sim'}:
                                kill_msg = f"{f_info['type']}->{r_info['type']} kill"
                                print(f"\n{kill_msg} for task {at['name']}")
                                jobstatus.write(f"[{kill_msg.upper()}] {at['name']}\n")
                                jobstatus.flush()
                                try:
                                    r_info['proc'].terminate()
                                except Exception:
                                    pass
                                killed_any = True
                    if killed_any:
                        at['killed'] = True

                # Check if all processes are now done
                all_done = True
                failed = False
                for p_info in at['processes']:
                    poll_status = p_info['proc'].poll()
                    if poll_status is None:
                        all_done = False
                    else:
                        if poll_status != 0:
                            failed = True
                
                if all_done:
                    for f in at['files']:
                        f.close()
                    duration = time.time() - at['time'] 
                    hours, rem = divmod(duration, 3600)
                    minutes, seconds = divmod(rem, 60)
                    duration_str = f"{int(hours)}h {int(minutes)}m {seconds:.2f}s"
                    
                    status = "FAILED" if failed else "SUCCESS"
                    if status == "SUCCESS":
                        finished_jobs += 1
                        
                    print(f"Done {finished_jobs}/{total_tasks}", end="\r", flush=True)
                    jobstatus.write(f"[DONE]  {at['name']} (Status: {status}, TAT: {duration_str})\n")
                    jobstatus.flush()
                    active_tasks.remove(at)

            # Fill available slots
            while task_queue:
                next_task = task_queue[0]
                slots_needed = 2 if (args.app_driven and next_task['pincmd']) else 1
                current_usage = get_cpu_usage(active_tasks)
                
                if current_usage + slots_needed <= args.jobs or current_usage == 0:
                    task = task_queue.pop(0)
                    jobstatus.write(f"[START] {task['name']}\n")
                    jobstatus.flush()
                    
                    if args.app_driven and task['pincmd']:
                        # Separate log for sim and pin
                        log_base, log_ext = os.path.splitext(task['log'])
                        sim_log = f"{log_base}_sim{log_ext}"
                        
                        f_sim = open(sim_log, "w")
                        
                        # Launch PIN job first, then SIM job
                        p_pin = subprocess.Popen(task['pincmd'], shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                        p_sim = subprocess.Popen(task['simcmd'], shell=True, stdout=f_sim, stderr=subprocess.STDOUT)
                        processes = [
                            {'proc': p_pin, 'type': 'pin'},
                            {'proc': p_sim, 'type': 'sim'}
                        ]
                        files = [f_sim]
                    else:
                        f_sim = open(task['log'], "w")
                        p_sim = subprocess.Popen(task['simcmd'], shell=True, stdout=f_sim, stderr=subprocess.STDOUT)
                        processes = [
                            {'proc': p_sim, 'type': 'sim'}
                        ]
                        files = [f_sim]
                        
                    active_tasks.append({
                        'name': task['name'],
                        'log': task['log'],
                        'files': files,
                        'processes': processes,
                        'time': time.time()
                    })
                else:
                    break

            time.sleep(0.5) # Prevent high CPU usage by the manager loop

    print("\nAll experiments completed.")

if __name__ == "__main__":
    main()
