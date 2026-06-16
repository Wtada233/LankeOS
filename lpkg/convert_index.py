#!/usr/bin/env python3
import sys

def convert_index(input_file, output_file):
    with open(input_file, 'r') as f:
        lines = f.readlines()

    with open(output_file, 'w') as f:
        for line in lines:
            line = line.strip()
            if not line or line.startswith('#'):
                f.write(line + '\n')
                continue
            
            parts = line.split('|')
            if len(parts) < 2:
                continue
            
            name = parts[0]
            versions_part = parts[1].split(',')
            deps = parts[2] if len(parts) > 2 else ""
            provides = parts[3] if len(parts) > 3 else ""
            
            for vh in versions_part:
                if ':' in vh:
                    v, h = vh.split(':', 1)
                    # Aggregated deps/provides from old format become per-version in new format
                    f.write(f"{name}|{v}:{h}:{deps}|{provides}\n")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: convert_index.py <old_index> <new_index>")
        sys.exit(1)
    convert_index(sys.argv[1], sys.argv[2])
