#!/bin/bash
# Monitor the SQLite database for new frames

DB_PATH="${1:-storage/dist_imaging.sqlite}"

if [ ! -f "$DB_PATH" ]; then
    echo "Database not found at $DB_PATH"
    exit 1
fi

echo "Monitoring $DB_PATH (press Ctrl+C to stop)"
echo ""

while true; do
    # Poll every few seconds so demo environments have basic observability.
    clear
    echo "=== Frame Count ==="
    sqlite3 "$DB_PATH" "SELECT COUNT(*) as total_frames FROM frames;"
    echo ""
    echo "=== Recent Frames (last 5) ==="
    sqlite3 -header -column "$DB_PATH" \
        "SELECT frame_id, keypoint_count, filename, created_at 
         FROM frames 
         ORDER BY id DESC 
         LIMIT 5;"
    echo ""
    echo "Last updated: $(date '+%H:%M:%S')"
    sleep 2
done

