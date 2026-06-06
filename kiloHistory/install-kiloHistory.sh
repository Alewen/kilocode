#!/bin/bash

SERVICE_NAME="kiloHistory"
USER_SERVICE_DIR="$HOME/.config/systemd/user"
SERVICE_FILE="$USER_SERVICE_DIR/$SERVICE_NAME.service"

mkdir -p "$USER_SERVICE_DIR"

cat > "$SERVICE_FILE" << 'EOF'
[Unit]
Description=Kilo History Viewer Service
After=default.target

[Service]
Type=simple
ExecStart=/usr/bin/python3 /var/www/html/kiloHistory/server.py
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload
systemctl --user enable "$SERVICE_NAME"

echo "Service '$SERVICE_NAME' installed successfully."
echo "Use 'systemctl --user start $SERVICE_NAME' to start"
echo "Use 'systemctl --user stop $SERVICE_NAME' to stop"
echo "Use 'systemctl --user status $SERVICE_NAME' to check status"
