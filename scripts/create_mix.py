import os
import re
import sys

def generate_homogeneous_mix(input_file, output_file, num_cores=8):
    if not os.path.exists(input_file):
        print(f"Error: Input file '{input_file}' not found.")
        return

    with open(input_file, 'r') as f:
        content = f.read()

    # Regex to pull out blocks of NAME, TRACE, and optional KNOBS
    # It dynamically captures everything after the '=' up to the newline
    pattern = r"NAME=([^\n]+)\nTRACE=([^\n]+)\nKNOBS=([^\n]*)"
    matches = re.findall(pattern, content)

    if not matches:
        print("No valid benchmark configurations found in the input file.")
        return

    output_lines = []

    for name, trace_path, knobs in matches:
        name = name.strip()
        trace_path = trace_path.strip()
        knobs = knobs.strip()

        # Clean the trace path to extract just the base filename if it has a path prefix
        # e.g., $(HERMES_TRACE)/605.mcf... -> 605.mcf...
        filename = trace_path.split('/')[-1]

        # Generate the homogeneous 8-core formats
        new_name = f"{num_cores}x{name}"
        new_trace = f"({filename})x{num_cores}"

        # Build the output block
        output_lines.append(f"NAME={new_name}")
        output_lines.append(f"TRACE={new_trace}")
        output_lines.append(f"KNOBS={knobs}\n")

    # Write the formatted payload out to the file
    with open(output_file, 'w') as f:
        f.write("\n".join(output_lines))

    print(f"Success! Homogeneous mix written to '{output_file}'")

# --- Command Line Execution ---
if __name__ == "__main__":
    # You can change these file names as needed
    infile = sys.argv[1]
    outfile = "homogeneous_8core_mix.txt"
    
    generate_homogeneous_mix(infile, outfile, num_cores=2)