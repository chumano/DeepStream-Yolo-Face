#!/bin/bash
PROCESS_NAME="deepstream"
LOG_FILE="usage.csv"

# Write header
echo "Timestamp,CPU_%,MEM_MB" > $LOG_FILE

while true; do
    # Get PID, CPU usage, and RSS (in KB)
    # rss is divided by 1024 to get MB
    STATS=$(ps -C "$PROCESS_NAME" -o pcpu,rss --no-headers | awk '{print $1 "," $2/1024}')
    
    if [ ! -z "$STATS" ]; then
        TIMESTAMP=$(date +"%Y-%m-%d %H:%M:%S")
        echo "$TIMESTAMP,$STATS" >> $LOG_FILE
    fi
    # Sleep for 30 second before the next check
    sleep 30
done