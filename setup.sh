#!/bin/bash
# Exit on any error
set -e

# Ensure script is run as root
if [ "$(id -u)" -ne 0 ]; then
    echo "Please run this script as root or using sudo."
    exit 1
fi

echo "=== Updating system ==="
apt-get update && apt-get upgrade -y

echo "=== Installing required packages for rAthena and web ==="
# NOTE: On Debian 13 (Trixie) / newer Ubuntu, 'ntp' was replaced by 'ntpsec'
# and 'libpcre3-dev' was replaced by 'libpcre2-dev'. rAthena builds fine
# against PCRE2. This script detects Debian 13+ and swaps package names
# automatically; older distros keep the legacy names.
. /etc/os-release 2>/dev/null || true

NTP_PKG="ntp"
PCRE_PKG="libpcre3-dev"

if [ "$ID" = "debian" ] && [ "${VERSION_ID:-0}" -ge 13 ] 2>/dev/null; then
    NTP_PKG="ntpsec"
    PCRE_PKG="libpcre2-dev"
elif ! apt-cache show libpcre3-dev >/dev/null 2>&1; then
    # Fallback: if libpcre3-dev isn't available for any reason, use PCRE2
    PCRE_PKG="libpcre2-dev"
fi

if ! apt-cache show "$NTP_PKG" >/dev/null 2>&1; then
    NTP_PKG="ntpsec"
fi

echo "Using NTP package: $NTP_PKG"
echo "Using PCRE package: $PCRE_PKG"

DEBIAN_FRONTEND=noninteractive apt-get install -y \
    git make g++ gcc cmake zlib1g-dev "$PCRE_PKG" screen dos2unix \
    libcurl4-gnutls-dev zip unzip gdb nano sudo curl wget openssl \
    apache2 libapache2-mod-php "$NTP_PKG" php php-mysql php-mbstring php-zip \
    php-gd php-json php-curl php-xml net-tools \
    libmariadb-dev libmariadb-dev-compat mariadb-server

echo "=== Fixing MySQL/MariaDB include path for rAthena ==="
if [ ! -d /usr/include/mysql ]; then
    ln -s /usr/include/mariadb /usr/include/mysql
    echo "Symlink /usr/include/mysql -> /usr/include/mariadb created"
else
    echo "Symlink /usr/include/mysql already exists, skipping"
fi

echo "=== Reconfiguring timezone silently ==="
/usr/sbin/dpkg-reconfigure -f noninteractive tzdata

echo "=== Enabling and starting MariaDB ==="
systemctl enable mariadb
systemctl start mariadb

# Prompt for MySQL/phpMyAdmin credentials
read -p "Enter MySQL/phpMyAdmin username to create: " DB_USER
read -s -p "Enter password for $DB_USER: " DB_PASS
echo
read -s -p "Confirm password: " DB_PASS2
echo
if [ "$DB_PASS" != "$DB_PASS2" ]; then
    echo "Passwords do not match. Exiting."
    exit 1
fi

echo "=== Creating MySQL user ==="
# Use socket auth via 'mysql' run as root (works even if root has no
# password set, which is the default on fresh MariaDB installs using
# unix_socket authentication).
mysql -u root <<MYSQL_SCRIPT
CREATE USER IF NOT EXISTS '$DB_USER'@'localhost' IDENTIFIED BY '$DB_PASS';
GRANT ALL PRIVILEGES ON *.* TO '$DB_USER'@'localhost' WITH GRANT OPTION;
FLUSH PRIVILEGES;
MYSQL_SCRIPT

echo "=== Installing phpMyAdmin manually (apt package is outdated/unmaintained) ==="
PHPMYADMIN_VERSION="5.2.1"
PMA_DIR="/usr/share/phpmyadmin"

if [ -d "$PMA_DIR" ]; then
    echo "Existing phpMyAdmin install found at $PMA_DIR, removing before reinstall"
    rm -rf "$PMA_DIR"
fi

cd /tmp
wget -q "https://files.phpmyadmin.net/phpMyAdmin/${PHPMYADMIN_VERSION}/phpMyAdmin-${PHPMYADMIN_VERSION}-all-languages.zip" -O phpmyadmin.zip
unzip -q phpmyadmin.zip
mv "phpMyAdmin-${PHPMYADMIN_VERSION}-all-languages" "$PMA_DIR"
rm -f phpmyadmin.zip

mkdir -p "$PMA_DIR/tmp"
chown -R www-data:www-data "$PMA_DIR/tmp"
chmod 770 "$PMA_DIR/tmp"

BLOWFISH_SECRET=$(openssl rand -base64 32)
cp "$PMA_DIR/config.sample.inc.php" "$PMA_DIR/config.inc.php"
sed -i "s|\$cfg\['blowfish_secret'\] = '';|\$cfg['blowfish_secret'] = '$BLOWFISH_SECRET';|" "$PMA_DIR/config.inc.php"

echo "=== Configuring Apache for phpMyAdmin ==="
cat > /etc/apache2/conf-available/phpmyadmin.conf <<APACHE_CONF
Alias /phpmyadmin $PMA_DIR

<Directory $PMA_DIR>
    Options FollowSymLinks
    DirectoryIndex index.php
    AllowOverride All
    Require all granted
</Directory>

<Directory $PMA_DIR/tmp>
    Require all denied
</Directory>
APACHE_CONF

a2enconf phpmyadmin
a2enmod rewrite
systemctl reload apache2
systemctl restart apache2

echo "=== Setup Complete ==="
IP_ADDRESS=$(hostname -I | awk '{print $1}')
echo "You can now access phpMyAdmin at: http://$IP_ADDRESS/phpmyadmin"
echo "Login with username: $DB_USER"
echo ""
echo "NOTE: '$DB_USER' was granted ALL PRIVILEGES WITH GRANT OPTION on *.* — fine for a local dev box,"
echo "but on anything internet-facing you should scope this down and put phpMyAdmin behind auth/HTTPS."