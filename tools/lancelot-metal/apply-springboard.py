"""Make SpringBoard's GPU detection follow the native Mali service."""
import argparse
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('source',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
text=a.source.read_text();anchor='''	if (access(REDWOOD_GL, R_OK) != 0) {
		return false;
	}'''
assert text.count(anchor)==1 and not a.output.exists()
replacement='''	/* Metal rendering is provided by LancelotMali. Do not classify it as
	 * software merely because the old RedwoodGPU service is absent. */
	gpu = IOServiceGetMatchingService(0, IOServiceMatching("LancelotMali"));
	if (gpu != 0) { IOObjectRelease(gpu); return true; }
'''+anchor
a.output.write_text(text.replace(anchor,replacement,1))
