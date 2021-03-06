#!/usr/bin/env python

import os, sys, glob, json

def usage():
	print sys.argv[0] + " PATH_TO_OUTPUT_FILES" + " EXPNAME"


def aggregate(expname, *file_names):
	need_server_data = True
	output = {}
	output["client_logs"] = []
        if expname == "karma":
                output["npost"] = 0
                output["nvote"] = 0
                output["ncomment"] = 0
                output["nread"] = 0
        else:
                output["nposts"] = 0
                output["nbackposts"] = 0
                output["nsubscribes"] = 0
                output["nchecks"] = 0
                output["nfull"] = 0
                output["nposts_read"] = 0
                output["nactive"] = 0
                output["nlogouts"] = 0
	output["user_time"] = 0
	output["system_time"] = 0
	output["wall_time"] = 0
	count = 0
	for f in file_names:
		fd=open(f)
		structured_obj = json.load(fd)
		output["client_logs"].append(structured_obj["log"])
		if "server_logs" in structured_obj and need_server_data:
			output["server_logs"] = structured_obj["server_logs"]
			need_server_data = False
                if expname == "karma":
                        output["npost"] += structured_obj["npost"]
                        output["nvote"] += structured_obj["nvote"]
                        output["ncomment"] += structured_obj["ncomment"]
                        output["nread"] += structured_obj["nread"]
                else:
                        output["nposts"] += structured_obj["nposts"]
                        output["nbackposts"] += structured_obj["nbackposts"]
                        output["nsubscribes"] += structured_obj["nsubscribes"]
                        output["nchecks"] += structured_obj["nchecks"]
                        output["nfull"] += structured_obj["nfull"]
                        output["nposts_read"] += structured_obj["nposts_read"]
                        output["nactive"] += structured_obj["nactive"]
                        output["nlogouts"] += structured_obj["nlogouts"]
		output["user_time"] += structured_obj["user_time"]
		output["system_time"] += structured_obj["system_time"]
		output["wall_time"] += structured_obj["wall_time"]
		fd.close()
	
	output["user_time"] /= float(len(file_names))
	output["system_time"] /= float(len(file_names)) 
	output["wall_time"] /= float(len(file_names)) 
	return output

def aggregate_dir(dir, expname):
	if dir[-1] != '/':
		dir += '/'
	files = glob.glob(dir + "output_app_*")
	files = [os.path.abspath(x) for x in files]
	combined_json = aggregate(expname, *files)
	fdout = open(dir + "aggregate_output_app.json", 'w')
	json.dump(combined_json, fdout, indent=4, separators=(',', ': '))	

if __name__ == "__main__":
	if len(sys.argv) != 3:
		usage()
		exit()
	base_path = sys.argv[1]
        expname = sys.argv[2]
	aggregate_dir(base_path, expname)
