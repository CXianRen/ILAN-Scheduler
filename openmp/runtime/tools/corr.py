#!/usr/bin/env python3
import re
import sys
from collections import defaultdict
import numpy as np

def parse_out_file(filename):
    tasks = {}
    with open(filename, "r") as f:
        lines = f.readlines()

    # Header regex: capture Task ID, Routine ID, CPU ID, and Thread ID.
    header_pattern = r'Counters for Task (0x[0-9a-fA-F]+) executing routine (0x[0-9a-fA-F]+) on CPU#(\d+)\s+\(T#(\d+)\):'
    param_pattern = r'-\s*(.*?)\s*=\s*([0-9]*\.?[0-9]+)'

    i = 0
    while i < len(lines):
        line = lines[i].strip()
        header_match = re.search(header_pattern, line)
        if header_match:
            task_id = header_match.group(1)
            routine_id = header_match.group(2)
            cpu_id = int(header_match.group(3))
            thread_id = int(header_match.group(4))
            params = {}
            i += 1  # Advance to parameter lines
            while i < len(lines) and lines[i].lstrip().startswith("-"):
                param_line = lines[i].strip()
                param_match = re.match(param_pattern, param_line)
                if param_match:
                    key = param_match.group(1)
                    value_str = param_match.group(2)
                    value = float(value_str) if '.' in value_str else int(value_str)
                    params[key] = value
                i += 1
            tasks[task_id] = {
                "routine": routine_id,
                "parameters": params,
                "cpu": cpu_id,
                "thread": thread_id
            }
        else:
            i += 1
    return tasks

def group_tasks_by_routine(tasks):
    routines = defaultdict(list)
    for task_id, info in tasks.items():
        routines[info["routine"]].append((task_id, info))
    return routines

def calculate_pearson(x, y):
    if len(x) < 2:
        return None
    corr_matrix = np.corrcoef(x, y)
    return corr_matrix[0, 1]

def format_table(rows, headers):
    # Determine the maximum width of each column.
    cols = list(zip(*([headers] + rows)))
    col_widths = [max(len(str(item)) for item in col) for col in cols]
    # Build format strings.
    row_format = " | ".join("{:<" + str(w) + "}" for w in col_widths)
    separator = "-+-".join("-" * w for w in col_widths)
    table_lines = []
    table_lines.append(row_format.format(*headers))
    table_lines.append(separator)
    for row in rows:
        table_lines.append(row_format.format(*row))
    return "\n".join(table_lines)

def write_results_to_file(routines, output_filename, combined_corr):
    with open(output_filename, "w") as outf:
        for routine_id, tasks_list in routines.items():
            outf.write(f"Routine {routine_id}:\n")
            # For the table, determine the union of parameter keys.
            # We'll always include Task ID, CPU, and T#.
            param_keys = set()
            for _, info in tasks_list:
                param_keys.update(info["parameters"].keys())
            # Optionally, you can keep a specific order of keys. Here, we sort them.
            sorted_params = sorted(param_keys)
            # Build table header.
            headers = ["Task ID", "CPU", "T#"] + sorted_params
            rows = []
            # Sort tasks by ascending CPU.
            tasks_sorted = sorted(tasks_list, key=lambda item: item[1]["cpu"])
            for task_id, info in tasks_sorted:
                row = [task_id, str(info["cpu"]), str(info["thread"])]
                # For each parameter column, get the value (or empty if missing).
                for key in sorted_params:
                    value = info["parameters"].get(key, "")
                    row.append(str(value))
                rows.append(row)
            table_str = format_table(rows, headers)
            outf.write(table_str + "\n\n")
            
            # Also output Pearson correlations per routine.
            # Exclude 'Execution time' from parameter keys.
            corr_keys = set(sorted_params)
            if "Execution time" in corr_keys:
                corr_keys.remove("Execution time")
            outf.write("Pearson correlation with Execution time:\n")
            for param in sorted(corr_keys):
                exec_times = []
                param_values = []
                for _, info in tasks_list:
                    params = info["parameters"]
                    if "Execution time" in params and param in params:
                        exec_times.append(params["Execution time"])
                        param_values.append(params[param])
                if len(exec_times) < 2:
                    corr = "N/A"
                else:
                    corr_val = calculate_pearson(exec_times, param_values)
                    corr = f"{corr_val:.6f}"
                outf.write(f"  {param}: {corr}\n")
            outf.write("\n")
        
        # Write the combined correlation for the selected routines.
        if combined_corr is not None:
            outf.write("Combined Pearson correlation for given routines:\n")
            for param, corr in combined_corr.items():
                outf.write(f"  {param}: {corr}\n")
            outf.write("\n")

def compute_combined_correlations(routines, selected_routine_ids):
    combined_tasks = []
    for routine_id in selected_routine_ids:
        if routine_id in routines:
            combined_tasks.extend(routines[routine_id])
    param_keys = set()
    for _, info in combined_tasks:
        param_keys.update(info["parameters"].keys())
    if "Execution time" in param_keys:
        param_keys.remove("Execution time")
    
    combined_corr = {}
    for param in sorted(param_keys):
        exec_times = []
        param_values = []
        for _, info in combined_tasks:
            params = info["parameters"]
            if "Execution time" in params and param in params:
                exec_times.append(params["Execution time"])
                param_values.append(params[param])
        if len(exec_times) < 2:
            combined_corr[param] = "N/A"
        else:
            corr_val = calculate_pearson(exec_times, param_values)
            combined_corr[param] = f"{corr_val:.6f}"
    return combined_corr

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python script.py <input_filename>")
        sys.exit(1)
    
    input_filename = sys.argv[1]
    tasks = parse_out_file(input_filename)
    routines = group_tasks_by_routine(tasks)
    
    # Selected routines for combined correlation.
    selected_routine_ids = list(routines.keys()) #['0x555f30f7cd70', '0x555f30f7ca60', '0x555f30f7ca80']
    combined_corr = None
    combined_corr = compute_combined_correlations(routines, selected_routine_ids)
    
    output_filename = "corr_out.txt"
    write_results_to_file(routines, output_filename, combined_corr)
    print(f"Results written to {output_filename}")
