#!/bin/bash
# Setup script for PPPoE testing environment
# Creates veth pair and PPPoE server for testing NetworkManager PPPoE connections

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Configuration
VETH_PAIR="veth0"
VETH_CLIENT="veth1"
SERVER_IP="10.67.15.1"
CLIENT_IP_RANGE="10.67.15.100-200"
PPPOE_USER="test"
PPPOE_PASS="test123"
SERVICE_NAME="dsl-test"
CONNECTION_UUID="610c3648-304e-40ff-bb2d-2d3da1244c04"

# Files
PPPOE_OPTIONS="/etc/ppp/pppoe-server-options"
CHAP_SECRETS="/etc/ppp/chap-secrets"
CHAP_SECRETS_BACKUP="/etc/ppp/chap-secrets.backup-$(date +%s)"
NM_CONNECTION="/etc/NetworkManager/system-connections/DSL-Test.nmconnection"

# Function to print colored messages
print_success() {
    echo -e "${GREEN}[✓]${NC} $1"
}

print_error() {
    echo -e "${RED}[✗]${NC} $1"
}

print_info() {
    echo -e "${YELLOW}[i]${NC} $1"
}

# Function to check if running as root
check_root() {
    if [ "$EUID" -ne 0 ]; then
        print_error "This script must be run as root"
        echo "Please run: sudo $0 $@"
        exit 1
    fi
}

# Function to detect package manager
detect_package_manager() {
    if command -v apt-get &> /dev/null; then
        echo "apt"
    elif command -v dnf &> /dev/null; then
        echo "dnf"
    elif command -v yum &> /dev/null; then
        echo "yum"
    else
        echo "unknown"
    fi
}

# Function to install packages
install_packages() {
    local pm=$(detect_package_manager)

    print_info "Checking required packages..."

    local packages_to_install=()

    # Check for pppoe
    if ! command -v pppoe-server &> /dev/null; then
        case $pm in
            apt)
                packages_to_install+=("pppoe")
                ;;
            dnf|yum)
                packages_to_install+=("rp-pppoe")
                ;;
        esac
    fi

    # Check for pppd
    if ! command -v pppd &> /dev/null; then
        case $pm in
            apt)
                packages_to_install+=("ppp")
                ;;
            dnf|yum)
                packages_to_install+=("ppp")
                ;;
        esac
    fi

    if [ ${#packages_to_install[@]} -eq 0 ]; then
        print_success "All required packages are already installed"
        return 0
    fi

    print_info "Installing packages: ${packages_to_install[*]}"

    case $pm in
        apt)
            apt-get update -qq
            apt-get install -y "${packages_to_install[@]}"
            ;;
        dnf)
            dnf install -y "${packages_to_install[@]}"
            ;;
        yum)
            yum install -y "${packages_to_install[@]}"
            ;;
        *)
            print_error "Unknown package manager. Please install manually:"
            echo "  - pppoe/rp-pppoe"
            echo "  - ppp/pppd"
            exit 1
            ;;
    esac

    print_success "Packages installed successfully"
}

# Function to create veth pair
create_veth_pair() {
    print_info "Creating veth pair: $VETH_PAIR <-> $VETH_CLIENT"

    # Remove existing veth pair if exists
    if ip link show $VETH_PAIR &> /dev/null; then
        print_info "Removing existing veth pair..."
        ip link delete $VETH_PAIR 2>/dev/null || true
    fi

    # Create veth pair
    ip link add $VETH_PAIR type veth peer name $VETH_CLIENT

    # Bring up both interfaces
    ip link set $VETH_PAIR up
    ip link set $VETH_CLIENT up

    # Assign IP to server side
    ip addr add $SERVER_IP/24 dev $VETH_PAIR

    # Make NetworkManager manage veth1 (client side)
    print_info "Configuring NetworkManager to manage $VETH_CLIENT"
    nmcli device set $VETH_CLIENT managed yes 2>/dev/null || true

    print_success "veth pair created: $VETH_PAIR (server) <-> $VETH_CLIENT (client)"
}

# Function to setup PPPoE server configuration
setup_pppoe_config() {
    print_info "Setting up PPPoE server configuration..."

    # Create pppoe-server-options file
    cat > $PPPOE_OPTIONS <<EOF
# PPPoE server options for testing
lcp-echo-interval 10
lcp-echo-failure 2
ms-dns 8.8.8.8
ms-dns 8.8.4.4
netmask 255.255.255.0
defaultroute
noipdefault
usepeerdns
EOF

    print_success "Created $PPPOE_OPTIONS"

    # Backup existing chap-secrets if not already backed up
    if [ -f "$CHAP_SECRETS" ] && [ ! -f "$CHAP_SECRETS_BACKUP" ]; then
        cp "$CHAP_SECRETS" "$CHAP_SECRETS_BACKUP"
        print_info "Backed up existing $CHAP_SECRETS to $CHAP_SECRETS_BACKUP"
    fi

    # Add test credentials to chap-secrets
    if ! grep -q "^$PPPOE_USER.*$PPPOE_PASS" "$CHAP_SECRETS" 2>/dev/null; then
        echo "$PPPOE_USER * $PPPOE_PASS *" >> $CHAP_SECRETS
        print_success "Added test credentials to $CHAP_SECRETS"
    else
        print_info "Test credentials already exist in $CHAP_SECRETS"
    fi

    # Set proper permissions
    chmod 600 $CHAP_SECRETS
}

# Function to create NetworkManager connection
create_nm_connection() {
    print_info "Creating NetworkManager DSL-Test connection..."

    # Remove existing connection if exists
    if nmcli conn show DSL-Test &> /dev/null; then
        print_info "Removing existing DSL-Test connection..."
        nmcli conn delete DSL-Test 2>/dev/null || true
    fi

    # Create the connection file
    cat > "$NM_CONNECTION" <<EOF
[connection]
id=DSL-Test
uuid=$CONNECTION_UUID
type=pppoe
autoconnect=false
interface-name=ppp0

[ethernet]

[pppoe]
parent=$VETH_CLIENT
password-flags=2
username=$PPPOE_USER

[ipv4]
method=auto

[ipv6]
addr-gen-mode=default
method=auto

[proxy]
EOF

    # Set proper permissions
    chmod 600 "$NM_CONNECTION"

    # Reload NetworkManager connections
    nmcli connection reload

    print_success "Created DSL-Test connection with password-flags=2"
    print_info "Connection will prompt for password when activated"
}

# Function to start PPPoE server
start_pppoe_server() {
    print_info "Starting PPPoE server on $VETH_PAIR..."

    # Kill any existing pppoe-server
    pkill -f "pppoe-server.*$VETH_PAIR" 2>/dev/null || true
    sleep 1

    # Start pppoe-server
    pppoe-server -I $VETH_PAIR -L $SERVER_IP -R $CLIENT_IP_RANGE -S $SERVICE_NAME &

    sleep 2

    # Check if server is running
    if pgrep -f "pppoe-server.*$VETH_PAIR" > /dev/null; then
        print_success "PPPoE server started successfully"
        print_info "Service name: $SERVICE_NAME"
        print_info "Username: $PPPOE_USER"
        print_info "Password: $PPPOE_PASS (will be prompted)"
    else
        print_error "Failed to start PPPoE server"
        exit 1
    fi
}

# Function to show status
show_status() {
    echo ""
    echo "========================================"
    echo "PPPoE Test Environment Status"
    echo "========================================"
    echo ""

    # Check veth interfaces
    if ip link show $VETH_PAIR &> /dev/null; then
        print_success "veth pair exists"
        ip addr show $VETH_PAIR | grep -E "^\s*(inet|link)" | sed 's/^/  /'
    else
        print_error "veth pair not found"
    fi

    echo ""

    # Check PPPoE server
    if pgrep -f "pppoe-server" > /dev/null; then
        print_success "PPPoE server is running"
        echo "  PID: $(pgrep -f pppoe-server)"
    else
        print_error "PPPoE server is not running"
    fi

    echo ""
    echo "========================================"
    echo "Test your connection with:"
    echo "  nmcli conn up DSL-Test"
    echo ""
    echo "Check connection status:"
    echo "  nmcli conn show DSL-Test"
    echo "  ip addr show ppp0"
    echo ""
    echo "To cleanup, run:"
    echo "  sudo $0 --cleanup"
    echo "========================================"
}

# Function to cleanup
cleanup() {
    print_info "Cleaning up PPPoE test environment..."

    # Stop PPPoE server
    if pgrep -f "pppoe-server" > /dev/null; then
        pkill -f "pppoe-server"
        print_success "Stopped PPPoE server"
    fi

    # Bring down PPPoE connection if active
    if nmcli conn show --active | grep -q "DSL-Test"; then
        nmcli conn down DSL-Test 2>/dev/null || true
        print_success "Brought down DSL-Test connection"
    fi

    # Remove NetworkManager connection
    if nmcli conn show DSL-Test &> /dev/null; then
        nmcli conn delete DSL-Test 2>/dev/null || true
        print_success "Removed DSL-Test connection"
    fi

    # Remove connection file if exists
    if [ -f "$NM_CONNECTION" ]; then
        rm -f "$NM_CONNECTION"
        print_success "Removed $NM_CONNECTION"
    fi

    # Remove veth pair
    if ip link show $VETH_PAIR &> /dev/null; then
        ip link delete $VETH_PAIR
        print_success "Removed veth pair"
    fi

    # Remove pppoe-server-options
    if [ -f "$PPPOE_OPTIONS" ]; then
        rm -f $PPPOE_OPTIONS
        print_success "Removed $PPPOE_OPTIONS"
    fi

    # Remove test credentials from chap-secrets
    if [ -f "$CHAP_SECRETS" ]; then
        sed -i "/^$PPPOE_USER.*$PPPOE_PASS/d" $CHAP_SECRETS
        print_success "Removed test credentials from $CHAP_SECRETS"

        # Restore backup if exists
        local latest_backup=$(ls -t /etc/ppp/chap-secrets.backup-* 2>/dev/null | head -1)
        if [ -n "$latest_backup" ]; then
            print_info "Backup file available at: $latest_backup"
        fi
    fi

    print_success "Cleanup completed"
}

# Main function
main() {
    if [ "$1" == "--cleanup" ] || [ "$1" == "--teardown" ]; then
        check_root "$@"
        cleanup
        exit 0
    fi

    if [ "$1" == "--status" ]; then
        show_status
        exit 0
    fi

    if [ "$1" == "--help" ] || [ "$1" == "-h" ]; then
        echo "Usage: $0 [OPTION]"
        echo ""
        echo "Setup PPPoE testing environment for NetworkManager"
        echo ""
        echo "Options:"
        echo "  (no option)    Setup and start PPPoE test environment"
        echo "  --cleanup      Remove PPPoE test environment"
        echo "  --teardown     Alias for --cleanup"
        echo "  --status       Show current status"
        echo "  --help, -h     Show this help message"
        echo ""
        exit 0
    fi

    check_root "$@"

    echo "========================================"
    echo "PPPoE Test Environment Setup"
    echo "========================================"
    echo ""

    install_packages
    create_veth_pair
    setup_pppoe_config
    create_nm_connection
    start_pppoe_server
    show_status
}

# Run main function
main "$@"
