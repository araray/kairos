#!/usr/bin/env bash

# Wrapper script for jobs to log stdout and stderr

if [[ -z KAIROS_JOB_NAME ]]; then
    job_name="$(basename $1)"
else
    job_name="$KAIROS_JOB_NAME"
fi


# Set variables
LOG_DIR="/av/logs/kairos/job_wrapper_logs" # Directory to store logs
LOG_FILE="${LOG_DIR}/job_${job_name}.log" # Default log file name (customize per job)

# Ensure the log directory exists
mkdir -p "$LOG_DIR"

# Get timestamp
TIMESTAMP=$(date +"%Y-%m-%d %H:%M:%S")

# Execute the command passed as arguments
{
    echo "[$TIMESTAMP] Starting job: $@"
    "$@" # Run the provided command or script
    rc=$? # Capture the return code
    # Get final timestamp
    TIMESTAMP=$(date +"%Y-%m-%d %H:%M:%S")
    echo "[$TIMESTAMP] Job completed."
    echo "---"
} >> "$LOG_FILE" 2>&1

exit $rc # Return the captured return code
