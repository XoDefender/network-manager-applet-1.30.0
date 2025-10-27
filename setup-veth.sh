#!/bin/bash
# Script to create and manage veth pair for PPPoE DSL connections with test server
# Usage: ./setup-veth.sh [start|stop|status|test-server]

set -e

VETH0="veth0"
VETH1="veth1"
PPPOE_SERVER_IP="10.67.15.1"
PPPOE_CLIENT_IP="10.67.15.100"
PPPOE_POOL="/tmp/pppoe-pool"
PPPOE_CONF="/etc/ppp/pppoe-server-options"
PPPOE_SECRETS="/etc/ppp/pap-secrets"
PPPOE_PID="/var/run/pppoe-server.pid"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Check if running as root
check_root() {
    if [ "$EUID" -ne 0 ]; then
        echo -e "${RED}Error: This script must be run as root${NC}"
        echo "Please run: sudo $0 $*"
        exit 1
    fi
}

# Check if interface exists
interface_exists() {
    ip link show "$1" &> /dev/null
}

# Create veth pair
create_veth() {
    echo -e "${YELLOW}Creating veth pair: $VETH0 <-> $VETH1${NC}"

    # Check if interfaces already exist
    if interface_exists "$VETH0" && interface_exists "$VETH1"; then
        echo -e "${GREEN}veth pair already exists${NC}"
        return 0
    fi

    # Remove existing interfaces if only one exists (cleanup partial state)
    if interface_exists "$VETH0"; then
        echo "Removing orphaned $VETH0"
        ip link delete "$VETH0" 2>/dev/null || true
    fi
    if interface_exists "$VETH1"; then
        echo "Removing orphaned $VETH1"
        ip link delete "$VETH1" 2>/dev/null || true
    fi

    # Create veth pair
    if ip link add "$VETH0" type veth peer name "$VETH1"; then
        echo -e "${GREEN}Successfully created veth pair${NC}"
    else
        echo -e "${RED}Failed to create veth pair${NC}"
        return 1
    fi

    # Bring interfaces up
    echo "Bringing up interfaces..."
    ip link set "$VETH0" up
    ip link set "$VETH1" up

    # Make veth1 managed by NetworkManager (needed for PPPoE)
    echo "Configuring NetworkManager to manage veth1..."
    if command -v nmcli &> /dev/null; then
        # Set veth1 as managed
        nmcli device set "$VETH1" managed yes 2>/dev/null || true
        # Create a dummy connection for veth1 if needed
        if ! nmcli connection show id "veth1-managed" &>/dev/null; then
            nmcli connection add type ethernet ifname "$VETH1" con-name "veth1-managed" autoconnect no 2>/dev/null || true
        fi

        # Ensure DSL-Test connection exists with correct configuration
        echo "Checking DSL-Test PPPoE connection..."
        if nmcli connection show id "DSL-Test" &>/dev/null; then
            # Update existing connection
            nmcli connection modify DSL-Test \
                pppoe.parent "$VETH1" \
                pppoe.username "test" \
                pppoe.password "test123" \
                pppoe.password-flags 0 \
                connection.autoconnect no 2>/dev/null || true
            echo "Updated DSL-Test connection"
        else
            # Create new DSL-Test connection
            nmcli connection add \
                type pppoe \
                con-name DSL-Test \
                ifname ppp0 \
                pppoe.parent "$VETH1" \
                pppoe.username "test" \
                pppoe.password "test123" \
                autoconnect no 2>/dev/null || true
            echo "Created DSL-Test connection"
        fi
    fi

    echo -e "${GREEN}veth interfaces are up and ready${NC}"
}

# Remove veth pair
remove_veth() {
    echo -e "${YELLOW}Removing veth pair${NC}"

    # Remove NetworkManager connection for veth1 if exists
    if command -v nmcli &> /dev/null; then
        nmcli connection delete "veth1-managed" 2>/dev/null || true
    fi

    if interface_exists "$VETH0"; then
        ip link delete "$VETH0"
        echo -e "${GREEN}Removed $VETH0 (and paired $VETH1)${NC}"
    elif interface_exists "$VETH1"; then
        ip link delete "$VETH1"
        echo -e "${GREEN}Removed $VETH1${NC}"
    else
        echo -e "${YELLOW}veth pair does not exist${NC}"
    fi
}

# Setup PPPoE test server
setup_pppoe_server() {
    echo -e "${YELLOW}Setting up PPPoE test server on $VETH0${NC}"

    # Check if pppoe-server is installed
    if ! command -v pppoe-server &> /dev/null; then
        echo -e "${RED}Error: pppoe-server is not installed${NC}"
        echo "Please install it with: sudo apt-get install pppoe"
        return 1
    fi

    # Assign IP to veth0 for PPPoE server
    echo "Assigning IP $PPPOE_SERVER_IP to $VETH0"
    ip addr flush dev "$VETH0"
    ip addr add "$PPPOE_SERVER_IP/24" dev "$VETH0"

    # Create IP pool file
    echo "Creating IP pool file"
    cat > "$PPPOE_POOL" << EOF
$PPPOE_CLIENT_IP
EOF

    # Backup existing PAP secrets if exists
    if [ -f "$PPPOE_SECRETS" ]; then
        cp "$PPPOE_SECRETS" "${PPPOE_SECRETS}.backup.$(date +%s)"
    fi

    # Create/update PAP secrets file for authentication
    echo "Creating authentication file"
    # Check if entry already exists
    if ! grep -q "^test\s" "$PPPOE_SECRETS" 2>/dev/null; then
        cat >> "$PPPOE_SECRETS" << EOF
# PPPoE test credentials
test        *         test123             *
EOF
        chmod 600 "$PPPOE_SECRETS"
    fi

    # Create pppoe-server options
    echo "Creating PPPoE server options"
    cat > "$PPPOE_CONF" << EOF
# PPPoE server options
require-pap
login
lcp-echo-interval 10
lcp-echo-failure 2
ms-dns 8.8.8.8
ms-dns 8.8.4.4
netmask 255.255.255.0
defaultroute
noipdefault
usepeerdns
noauth
EOF

    # Stop existing pppoe-server if running
    if [ -f "$PPPOE_PID" ]; then
        OLD_PID=$(cat "$PPPOE_PID")
        if kill -0 "$OLD_PID" 2>/dev/null; then
            echo "Stopping existing PPPoE server (PID: $OLD_PID)"
            kill "$OLD_PID"
            sleep 1
        fi
        rm -f "$PPPOE_PID"
    fi

    # Kill any orphaned pppoe-server processes
    pkill -f "pppoe-server.*$VETH0" 2>/dev/null || true
    sleep 1

    # Start pppoe-server
    echo "Starting PPPoE server..."
    pppoe-server -I "$VETH0" -L "$PPPOE_SERVER_IP" -R "$PPPOE_CLIENT_IP" \
                 -N 10 -O "$PPPOE_CONF" -F &

    PPPOE_SERVER_PID=$!
    echo $PPPOE_SERVER_PID > "$PPPOE_PID"

    sleep 2

    if kill -0 "$PPPOE_SERVER_PID" 2>/dev/null; then
        echo -e "${GREEN}PPPoE server started successfully (PID: $PPPOE_SERVER_PID)${NC}"
        echo -e "${BLUE}Test credentials: username='test', password='test123'${NC}"
        echo -e "${BLUE}Client should connect on $VETH1${NC}"
    else
        echo -e "${RED}Failed to start PPPoE server${NC}"
        echo "Check logs with: journalctl -xe | tail -20"
        rm -f "$PPPOE_PID"
        return 1
    fi
}

# Stop PPPoE test server
stop_pppoe_server() {
    echo -e "${YELLOW}Stopping PPPoE test server${NC}"

    if [ -f "$PPPOE_PID" ]; then
        PID=$(cat "$PPPOE_PID")
        if kill -0 "$PID" 2>/dev/null; then
            kill "$PID"
            echo -e "${GREEN}PPPoE server stopped${NC}"
        else
            echo -e "${YELLOW}PPPoE server not running${NC}"
        fi
        rm -f "$PPPOE_PID"
    else
        # Try to find and kill pppoe-server process
        pkill -f "pppoe-server.*$VETH0" && echo -e "${GREEN}PPPoE server stopped${NC}" || echo -e "${YELLOW}No PPPoE server found${NC}"
    fi

    # Clean up temporary files
    rm -f "$PPPOE_POOL"

    # Remove test credentials from PAP secrets if they exist
    if [ -f "$PPPOE_SECRETS" ]; then
        sed -i '/# PPPoE test credentials/d' "$PPPOE_SECRETS"
        sed -i '/^test\s.*test123/d' "$PPPOE_SECRETS"
    fi
}

# Show status
show_status() {
    echo -e "${YELLOW}=== veth Interface Status ===${NC}"

    for iface in "$VETH0" "$VETH1"; do
        if interface_exists "$iface"; then
            state=$(ip link show "$iface" | grep -oP '(?<=state )[^ ]+' || echo "UNKNOWN")
            echo -e "${GREEN}✓${NC} $iface: $state"
            ip addr show "$iface" | grep -E "^\s+inet" | sed 's/^/  /' || echo "  (no IP address)"
        else
            echo -e "${RED}✗${NC} $iface: NOT FOUND"
        fi
    done

    echo ""
    echo -e "${YELLOW}=== PPPoE Server Status ===${NC}"
    if [ -f "$PPPOE_PID" ]; then
        PID=$(cat "$PPPOE_PID")
        if kill -0 "$PID" 2>/dev/null; then
            echo -e "${GREEN}✓${NC} PPPoE server running (PID: $PID)"
        else
            echo -e "${RED}✗${NC} PPPoE server not running (stale PID file)"
        fi
    else
        if pgrep -f "pppoe-server.*$VETH0" > /dev/null; then
            echo -e "${YELLOW}⚠${NC} PPPoE server running (no PID file)"
        else
            echo -e "${RED}✗${NC} PPPoE server not running"
        fi
    fi

    echo ""
    echo -e "${YELLOW}=== PPPoE Connections ===${NC}"
    nmcli connection show | grep pppoe || echo "No PPPoE connections found"

    echo ""
    echo -e "${YELLOW}=== Active PPP Sessions ===${NC}"
    if [ -d /var/run ]; then
        ls -la /var/run/ppp*.pid 2>/dev/null || echo "No active PPP sessions"
    fi
}

# Main script
case "${1:-start}" in
    start)
        check_root
        create_veth
        echo ""
        show_status
        ;;
    stop)
        check_root
        stop_pppoe_server
        remove_veth
        ;;
    status)
        show_status
        ;;
    restart)
        check_root
        stop_pppoe_server
        remove_veth
        sleep 1
        create_veth
        echo ""
        show_status
        ;;
    test-server)
        check_root
        # Ensure veth pair exists
        if ! interface_exists "$VETH0" || ! interface_exists "$VETH1"; then
            create_veth
            echo ""
        fi
        setup_pppoe_server
        echo ""
        show_status
        echo ""
        echo -e "${BLUE}========================================${NC}"
        echo -e "${BLUE}Test PPPoE Environment Ready!${NC}"
        echo -e "${BLUE}========================================${NC}"
        echo -e "Now you can connect using nm-applet with:"
        echo -e "  Connection: DSL-Test"
        echo -e "  Username: test"
        echo -e "  Password: test123"
        echo -e "  Interface: $VETH1"
        echo ""
        ;;
    stop-server)
        check_root
        stop_pppoe_server
        ;;
    full-test)
        check_root
        echo -e "${BLUE}Setting up complete PPPoE test environment...${NC}"
        echo ""

        # Stop everything first
        stop_pppoe_server
        remove_veth
        sleep 1

        # Create fresh setup
        create_veth
        echo ""
        setup_pppoe_server
        echo ""
        show_status

        echo ""
        echo -e "${GREEN}========================================${NC}"
        echo -e "${GREEN}Full Test Environment Ready!${NC}"
        echo -e "${GREEN}========================================${NC}"
        echo -e "Connect using nm-applet:"
        echo -e "  1. Click on nm-applet tray icon"
        echo -e "  2. Select 'DSL-Test' connection"
        echo -e "  3. Enter credentials:"
        echo -e "     Username: ${YELLOW}test${NC}"
        echo -e "     Password: ${YELLOW}test123${NC}"
        echo ""
        ;;
    *)
        echo "Usage: $0 {start|stop|status|restart|test-server|stop-server|full-test}"
        echo ""
        echo "Commands:"
        echo "  start        - Create and bring up veth pair (default)"
        echo "  stop         - Stop server and remove veth pair"
        echo "  status       - Show current status"
        echo "  restart      - Stop server, remove and recreate veth pair"
        echo "  test-server  - Setup PPPoE test server on veth0"
        echo "  stop-server  - Stop PPPoE test server"
        echo "  full-test    - Complete test setup (veth + PPPoE server)"
        echo ""
        echo "Quick Start for Testing:"
        echo "  sudo $0 full-test"
        exit 1
        ;;
esac

exit 0
