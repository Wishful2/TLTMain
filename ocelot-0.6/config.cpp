#include "config.h"

config::config() {
	host = "127.0.0.1";
	port = 34000;
	max_connections = 512;
	max_read_buffer = 4096;
	timeout_interval = 20;
	schedule_interval = 3;
	max_middlemen = 5000;

	announce_interval = 1800;
	peers_timeout = 2700; //Announce interval * 1.5

	reap_peers_interval = 1800;
	del_reason_lifetime = 604800;

	// MySQL
	mysql_db = "gazelle";
	mysql_host = "127.0.0.1:3306";
	mysql_username = "gazelle";
	mysql_password = "7yr10n!!$$";

	// Site communication
	site_host = "localhost";
	site_password = "98SEne3AbAqeStEsPecraPhUSpuBR9h6"; // MUST BE 32 CHARS
	site_path = ""; // If the site is not running under the domain root
}
