#!/bin/sh
# Start the server. It reads config.ini and keeps its database and logs under bin/.
cd "$(dirname "$0")/bin" && exec ./champions-server "$@"
