#include <cmath>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <sstream>
#include <list>
#include <vector>
#include <set>
#include <algorithm>

#include <netinet/in.h>
#include <arpa/inet.h>

#include "ocelot.h"
#include "config.h"
#include "db.h"
#include "worker.h"
#include "misc_functions.h"
#include "site_comm.h"

#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread/locks.hpp>
#include <boost/bind.hpp>
#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/gzip.hpp>

//---------- Worker - does stuff with input
worker::worker(torrent_list &torrents, user_list &users, std::vector<std::string> &_whitelist, config * conf_obj, mysql * db_obj, site_comm * sc) : torrents_list(torrents), users_list(users), whitelist(_whitelist), conf(conf_obj), db(db_obj), s_comm(sc)
{
	status = OPEN;
}
bool worker::signal(int sig) {
	if (status == OPEN) {
		status = CLOSING;
		std::cout << "closing tracker... press Ctrl-C again to terminate" << std::endl;
		return false;
	} else if (status == CLOSING) {
		std::cout << "shutting down uncleanly" << std::endl;
		return true;
	} else {
		return false;
	}
}
std::string worker::work(std::string &input, std::string &ip, bool &gzip) {
	unsigned int input_length = input.length();

	//---------- Parse request - ugly but fast. Using substr exploded.
	if (input_length < 60) { // Way too short to be anything useful
		return error("GET string too short");
	}

	size_t pos = 5; // skip 'GET /'

	// Get the passkey
	std::string passkey;
	passkey.reserve(32);
	if (input[37] != '/') {
		/* This didn't work as intended. We want the crawler to download the meta tag
		if (input.substr(5, 10) == "robots.txt") {
			// Let's just hope that no crawler has a / at pos 37
			return "User-agent: *\nDisallow: /";
		}*/
		return error("Malformed announce");
	}

	for (; pos < 37; pos++) {
		passkey.push_back(input[pos]);
	}

	pos = 38;

	// Get the action
	enum action_t {
		INVALID = 0, ANNOUNCE, SCRAPE, UPDATE
	};
	action_t action = INVALID;

	switch (input[pos]) {
		case 'a':
			action = ANNOUNCE;
			pos += 8;
			break;
		case 's':
			action = SCRAPE;
			pos += 6;
			break;
		case 'u':
			action = UPDATE;
			pos += 6;
			break;
	}
	if (action == INVALID) {
		return error("invalid action");
	}

	if (input[pos] != '?') {
		// No parameters given. Probably means we're not talking to a torrent client
		return "<html><head><meta name=\"robots\" content=\"noindex, nofollow\" /></head><body>Nothing to see here</body></html>";
	}

	if ((status != OPEN) && (action != UPDATE)) {
		return error("The tracker is temporarily unavailable.");
	}

	// Parse URL params
	std::list<std::string> infohashes; // For scrape only

	std::map<std::string, std::string> params;
	std::string key;
	std::string value;
	bool parsing_key = true; // true = key, false = value

	pos++; // Skip the '?'
	for (; pos < input_length; ++pos) {
		if (input[pos] == '=') {
			parsing_key = false;
		} else if (input[pos] == '&' || input[pos] == ' ') {
			parsing_key = true;
			if (action == SCRAPE && key == "info_hash") {
				infohashes.push_back(value);
			} else {
				params[key] = value;
			}
			key.clear();
			value.clear();
			if (input[pos] == ' ') {
				break;
			}
		} else {
			if (parsing_key) {
				key.push_back(input[pos]);
			} else {
				value.push_back(input[pos]);
			}
		}
	}

	pos += 10; // skip 'HTTP/1.1' - should probably be +=11, but just in case a client doesn't send \r

	// Parse headers
	std::map<std::string, std::string> headers;
	parsing_key = true;
	bool found_data = false;

	for (; pos < input_length; ++pos) {
		if (input[pos] == ':') {
			parsing_key = false;
			++pos; // skip space after :
		} else if (input[pos] == '\n' || input[pos] == '\r') {
			parsing_key = true;

			if (found_data) {
				found_data = false; // dodge for getting around \r\n or just \n
				std::transform(key.begin(), key.end(), key.begin(), ::tolower);
				headers[key] = value;
				key.clear();
				value.clear();
			}
		} else {
			found_data = true;
			if (parsing_key) {
				key.push_back(input[pos]);
			} else {
				value.push_back(input[pos]);
			}
		}
	}



	if (action == UPDATE) {
		if (passkey == conf->site_password) {
			return update(params);
		} else {
			return error("Authentication failure");
		}
	}

	// Either a scrape or an announce

	user_list::iterator u = users_list.find(passkey);
	if (u == users_list.end()) {
		return error("Passkey not found");
	}

	if (action == ANNOUNCE) {
		boost::mutex::scoped_lock lock(db->torrent_list_mutex);
		// Let's translate the infohash into something nice
		// info_hash is a url encoded (hex) base 20 number
		std::string info_hash_decoded = hex_decode(params["info_hash"]);
		torrent_list::iterator tor = torrents_list.find(info_hash_decoded);
		if (tor == torrents_list.end()) {
			boost::mutex::scoped_lock lock(del_reasons_lock);
			auto msg = del_reasons.find(info_hash_decoded);
			if (msg != del_reasons.end()) {
				if (msg->second.reason != -1) {
					return error("Unregistered torrent: " + get_del_reason(msg->second.reason));
				} else {
					return error("Unregistered torrent");
				}
			} else {
				return error("Unregistered torrent");
			}
		}
		return announce(tor->second, u->second, params, headers, ip, gzip);
	} else {
		return scrape(infohashes, headers, gzip);
	}
}

std::string worker::error(std::string err) {
	std::string output = "d14:failure reason";
	output += inttostr(err.length());
	output += ':';
	output += err;
	output += 'e';
	return output;
}

std::string worker::warning(std::string msg) {
	std::string output = "15:warning message";
	output += inttostr(msg.length());
	output += ':';
	output += msg;
	return output;
}

std::string worker::announce(torrent &tor, user &u, std::map<std::string, std::string> &params, std::map<std::string, std::string> &headers, std::string &ip, bool &gzip) {
	cur_time = time(NULL);

	if (params["compact"] != "1") {
		return error("Your client does not support compact announces");
	}

	int64_t left = strtolonglong(params["left"]);
	int64_t uploaded = std::max((int64_t)0, strtolonglong(params["uploaded"]));
	int64_t downloaded = std::max((int64_t)0, strtolonglong(params["downloaded"]));
	int64_t corrupt = strtolonglong(params["corrupt"]);

	int snatches = 0; // This is the value that gets sent to the database on a snatch
	int active = 1; // This is the value that marks a peer as active/inactive in the database
	bool inserted = false; // If we insert the peer as opposed to update
	bool update_torrent = false; // Whether or not we should update the torrent in the DB
	bool completed_torrent = false; // Whether or not the current announcement is a snatch
	bool stopped_torrent = false; // Was the torrent just stopped?
	bool expire_token = false; // Whether or not to expire a token after torrent completion
	bool peer_changed = false; // Whether or not the peer is new or has changed since the last announcement

	std::map<std::string, std::string>::const_iterator peer_id_iterator = params.find("peer_id");
	if (peer_id_iterator == params.end()) {
		return error("No peer ID");
	}
	std::string peer_id = peer_id_iterator->second;
	peer_id = hex_decode(peer_id);

	if (whitelist.size() > 0) {
		bool found = false; // Found client in whitelist?
		for (unsigned int i = 0; i < whitelist.size(); i++) {
			if (peer_id.find(whitelist[i]) == 0) {
				found = true;
				break;
			}
		}

		if (!found) {
			return error("Your client is not on the whitelist");
		}
	}
	if (params["event"] == "completed") {
		// Don't update <snatches> here as we may decide to use other conditions later on
		completed_torrent = true;
	} else if (params["event"] == "stopped") {
		stopped_torrent = true;
		peer_changed = true;
		update_torrent = true;
		active = 0;
	}

	peer * p;
	peer_list::iterator i;
	// Insert/find the peer in the torrent list
	if (left > 0) {
		i = tor.leechers.find(peer_id);
		if (i == tor.leechers.end()) {
			peer new_peer;
			std::pair<peer_list::iterator, bool> insert
			= tor.leechers.insert(std::pair<std::string, peer>(peer_id, new_peer));

			p = &(insert.first->second);
			inserted = true;
		} else {
			p = &i->second;
		}
	} else if (completed_torrent) {
		i = tor.leechers.find(peer_id);
		if (i == tor.leechers.end()) {
			peer new_peer;
			std::pair<peer_list::iterator, bool> insert
			= tor.seeders.insert(std::pair<std::string, peer>(peer_id, new_peer));

			p = &(insert.first->second);
			inserted = true;
		} else {
			p = &i->second;
		}
	} else {
		i = tor.seeders.find(peer_id);
		if (i == tor.seeders.end()) {
			i = tor.leechers.find(peer_id);
			if (i == tor.leechers.end()) {
				peer new_peer;
				std::pair<peer_list::iterator, bool> insert
				= tor.seeders.insert(std::pair<std::string, peer>(peer_id, new_peer));

				p = &(insert.first->second);
				inserted = true;
			} else {
				p = &i->second;
				std::pair<peer_list::iterator, bool> insert
				= tor.seeders.insert(std::pair<std::string, peer>(peer_id, *p));
				tor.leechers.erase(peer_id);
				if (downloaded > 0) {
					std::cout << "Found unreported snatch from user " << u.id << " on torrent " << tor.id << std::endl;
				}
				p = &(insert.first->second);
				peer_changed = true;
//				completed_torrent = true; // Not sure if we want to do this. Will cause massive spam for weird clients (e.g. µTorrent 3 and Deluge)
			}
		} else {
			p = &i->second;
		}

		tor.last_seeded = cur_time;
	}

	int64_t upspeed = 0;
	int64_t downspeed = 0;
	if (inserted || params["event"] == "started") {
		//New peer on this torrent
		update_torrent = true;
		p->userid = u.id;
		p->first_announced = cur_time;
		p->last_announced = 0;
		p->uploaded = uploaded;
		p->downloaded = downloaded;
		p->corrupt = corrupt;
		p->announces = 1;
		peer_changed = true;
	} else if (uploaded < p->uploaded || downloaded < p->downloaded) {
		p->announces++;
		p->uploaded = uploaded;
		p->downloaded = downloaded;
		peer_changed = true;
	} else {
		int64_t uploaded_change = 0;
		int64_t downloaded_change = 0;
		int64_t corrupt_change = 0;
		p->announces++;

		if (uploaded != p->uploaded) {
			uploaded_change = uploaded - p->uploaded;
			p->uploaded = uploaded;
		}
		if (downloaded != p->downloaded) {
			downloaded_change = downloaded - p->downloaded;
			p->downloaded = downloaded;
		}
		if (corrupt != p->corrupt) {
			corrupt_change = corrupt - p->corrupt;
			p->corrupt = corrupt;
			tor.balance -= corrupt_change;
			update_torrent = true;
		}
		peer_changed = peer_changed || uploaded_change || downloaded_change || corrupt_change;

		if (uploaded_change || downloaded_change) {
			tor.balance += uploaded_change;
			tor.balance -= downloaded_change;
			update_torrent = true;

			if (cur_time > p->last_announced) {
				upspeed = uploaded_change / (cur_time - p->last_announced);
				downspeed = downloaded_change / (cur_time - p->last_announced);
			}
			std::set<int>::iterator sit = tor.tokened_users.find(u.id);
			if (tor.free_torrent == NEUTRAL) {
				downloaded_change = 0;
				uploaded_change = 0;
			} else if (tor.free_torrent == FREE || sit != tor.tokened_users.end()) {
				if (sit != tor.tokened_users.end()) {
					expire_token = true;
					std::stringstream record;
					record << '(' << u.id << ',' << tor.id << ',' << downloaded_change << ')';
					std::string record_str = record.str();
					db->record_token(record_str);
				}
				downloaded_change = 0;
			}

			if (uploaded_change || downloaded_change) {
				std::stringstream record;
				record << '(' << u.id << ',' << uploaded_change << ',' << downloaded_change << ')';
				std::string record_str = record.str();
				db->record_user(record_str);
			}
		}
	}
	p->left = left;

	std::map<std::string, std::string>::const_iterator param_ip = params.find("ip");
	if (param_ip != params.end()) {
		ip = param_ip->second;
	} else {
		param_ip = params.find("ipv4");
		if (param_ip != params.end()) {
			ip = param_ip->second;
		}
	}

	unsigned int port = strtolong(params["port"]);
	// Generate compact ip/port string
	if (inserted || port != p->port || ip != p->ip) {
		p->port = port;
		p->ip = ip;
		p->ip_port = "";
		char x = 0;
		bool invalid_ip = false;
		for (size_t pos = 0, end = ip.length(); pos < end; pos++) {
			if (ip[pos] == '.') {
				p->ip_port.push_back(x);
				x = 0;
				continue;
			} else if (!isdigit(ip[pos])) {
				invalid_ip = true;
				break;
			}
			x = x * 10 + ip[pos] - '0';
		}
		if (!invalid_ip) {
			p->ip_port.push_back(x);
			p->ip_port.push_back(port >> 8);
			p->ip_port.push_back(port & 0xFF);
		}
		if (p->ip_port.length() != 6) {
			p->ip_port.clear();
			invalid_ip = true;
		}
		p->invalid_ip = invalid_ip;
	}

	// Update the peer
	p->last_announced = cur_time;
	p->visible = peer_is_visible(&u, p);

	// Add peer data to the database
	std::stringstream record;
	if (peer_changed) {
		record << '(' << u.id << ',' << tor.id << ',' << active << ',' << uploaded << ',' << downloaded << ',' << upspeed << ',' << downspeed << ',' << left << ',' << corrupt << ',' << (cur_time - p->first_announced) << ',' << p->announces << ',';
		std::string record_str = record.str();
		std::string record_ip;
		if (u.protect_ip) {
			record_ip = "";
		} else {
			record_ip = ip;
		}
		db->record_peer(record_str, record_ip, peer_id, headers["user-agent"]);
	} else {
		record << '(' << tor.id << ',' << (cur_time - p->first_announced) << ',' << p->announces << ',';
		std::string record_str = record.str();
		db->record_peer(record_str, peer_id);
	}

	// Select peers!
	unsigned int numwant;
	std::map<std::string, std::string>::const_iterator param_numwant = params.find("numwant");
	if (param_numwant == params.end()) {
		numwant = 50;
	} else {
		numwant = std::min(50l, strtolong(param_numwant->second));
	}

	if (stopped_torrent) {
		numwant = 0;
		if (left > 0) {
			if (tor.leechers.erase(peer_id) == 0) {
				std::cout << "Tried and failed to remove seeder from torrent " << tor.id << std::endl;
			}
		} else {
			if (tor.seeders.erase(peer_id) == 0) {
				std::cout << "Tried and failed to remove leecher from torrent " << tor.id << std::endl;
			}
		}
	} else if (completed_torrent) {
		snatches = 1;
		update_torrent = true;
		tor.completed++;

		std::stringstream record;
		std::string record_ip;
		if (u.protect_ip) {
			record_ip = "";
		} else {
			record_ip = ip;
		}
		record << '(' << u.id << ',' << tor.id << ',' << cur_time << ", '" << record_ip << "')";
		std::string record_str = record.str();
		db->record_snatch(record_str);

		// User is a seeder now!
		if (!inserted) {
			tor.seeders.insert(std::pair<std::string, peer>(peer_id, *p));
			tor.leechers.erase(peer_id);
		}
		if (expire_token) {
			s_comm->expire_token(tor.id, u.id);
			tor.tokened_users.erase(u.id);
		}
		// do cache expire
	} else if (!u.can_leech && left > 0) {
		numwant = 0;
	}

	std::string peers;
	if (numwant > 0) {
		peers.reserve(numwant*6);
		unsigned int found_peers = 0;
		if (left > 0) { // Show seeders to leechers first
			if (tor.seeders.size() > 0) {
				// We do this complicated stuff to cycle through the seeder list, so all seeders will get shown to leechers

				// Find out where to begin in the seeder list
				peer_list::const_iterator i;
				if (tor.last_selected_seeder == "") {
					i = tor.seeders.begin();
				} else {
					i = tor.seeders.find(tor.last_selected_seeder);
					if (i == tor.seeders.end() || ++i == tor.seeders.end()) {
						i = tor.seeders.begin();
					}
				}

				// Find out where to end in the seeder list
				peer_list::const_iterator end;
				if (i == tor.seeders.begin()) {
					end = tor.seeders.end();
				} else {
					end = i;
					if (--end == tor.seeders.begin()) {
						end++;
						i++;
					}
				}

				// Add seeders
				while(i != end && found_peers < numwant) {
					if (i == tor.seeders.end()) {
						i = tor.seeders.begin();
					}
					// Don't show users themselves
					if (i->second.userid == p->userid || !i->second.visible) {
						i++;
						continue;
					}
					peers.append(i->second.ip_port);
					found_peers++;
					tor.last_selected_seeder = i->first;
					i++;
				}
			}

			if (found_peers < numwant && tor.leechers.size() > 1) {
				for (peer_list::const_iterator i = tor.leechers.begin(); i != tor.leechers.end() && found_peers < numwant; i++) {
					// Don't show users themselves or leech disabled users
					if (i->second.ip_port == p->ip_port || i->second.userid == p->userid || !i->second.visible) {
						continue;
					}
					found_peers++;
					peers.append(i->second.ip_port);
				}

			}
		} else if (tor.leechers.size() > 0) { // User is a seeder, and we have leechers!
			for (peer_list::const_iterator i = tor.leechers.begin(); i != tor.leechers.end() && found_peers < numwant; i++) {
				// Don't show users themselves or leech disabled users
				if (i->second.userid == p->userid || !i->second.visible) {
						continue;
				}
				found_peers++;
				peers.append(i->second.ip_port);
			}
		}
	}
	
	if (update_torrent || tor.last_flushed + 3600 < cur_time) {
		tor.last_flushed = cur_time;
		
		std::stringstream record;
		record << '(' << tor.id << ',' << tor.seeders.size() << ',' << tor.leechers.size() << ',' << snatches << ',' << tor.balance << ')';
		std::string record_str = record.str();
		db->record_torrent(record_str);
	}
	
	if (!u.can_leech && left > 0) {
		return error("Access denied, leeching forbidden");
	}

	std::string response = "d8:completei";
	response.reserve(350);
	response += inttostr(tor.seeders.size());
	response += "e10:downloadedi";
	response += inttostr(tor.completed);
	response += "e10:incompletei";
	response += inttostr(tor.leechers.size());
	response += "e8:intervali";
	response += inttostr(conf->announce_interval+std::min((size_t)600, tor.seeders.size())); // ensure a more even distribution of announces/second
	response += "e12:min intervali";
	response += inttostr(conf->announce_interval);
	response += "e5:peers";
	if (peers.length() == 0) {
		response += "0:";
	} else {
		response += inttostr(peers.length());
		response += ":";
		response += peers;
	}
	if (p->invalid_ip) {
		response += warning("Illegal character found in IP address. IPv6 is not supported");
	}
	response += "e";

	/* gzip compression actually makes announce returns larger from our
	 * testing. Feel free to enable this here if you'd like but be aware of
	 * possibly inflated return size
	if (headers["accept-encoding"] == "gzip") {
		std::stringstream ss, zss;
		ss << response;
		boost::iostreams::filtering_streambuf<boost::iostreams::input> in;
		in.push(boost::iostreams::gzip_compressor());
		in.push(ss);
		boost::iostreams::copy(in, zss);
		response = zss.str();
		gzip = true;
	}*/

	return response;
}

std::string worker::scrape(const std::list<std::string> &infohashes, std::map<std::string, std::string> &headers, bool &gzip) {
	std::string output = "d5:filesd";
	for (std::list<std::string>::const_iterator i = infohashes.begin(); i != infohashes.end(); i++) {
		std::string infohash = *i;
		infohash = hex_decode(infohash);

		torrent_list::iterator tor = torrents_list.find(infohash);
		if (tor == torrents_list.end()) {
			continue;
		}
		torrent *t = &(tor->second);

		output += inttostr(infohash.length());
		output += ':';
		output += infohash;
		output += "d8:completei";
		output += inttostr(t->seeders.size());
		output += "e10:incompletei";
		output += inttostr(t->leechers.size());
		output += "e10:downloadedi";
		output += inttostr(t->completed);
		output += "ee";
	}
	output+="ee";

	if (headers["accept-encoding"].find("gzip") != std::string::npos) {
		std::stringstream ss, zss;
		ss << output;
		boost::iostreams::filtering_streambuf<boost::iostreams::input> in;
		in.push(boost::iostreams::gzip_compressor());
		in.push(ss);
		boost::iostreams::copy(in, zss);
		output = zss.str();
		gzip = true;
	}

	return output;
}

//TODO: Restrict to local IPs
std::string worker::update(std::map<std::string, std::string> &params) {
	if (params["action"] == "change_passkey") {
		std::string oldpasskey = params["oldpasskey"];
		std::string newpasskey = params["newpasskey"];
		user_list::iterator i = users_list.find(oldpasskey);
		if (i == users_list.end()) {
			std::cout << "No user with passkey " << oldpasskey << " exists when attempting to change passkey to " << newpasskey << std::endl;
		} else {
			users_list[newpasskey] = i->second;;
			users_list.erase(oldpasskey);
			std::cout << "Changed passkey from " << oldpasskey << " to " << newpasskey << " for user " << i->second.id << std::endl;
		}
	} else if (params["action"] == "add_torrent") {
		torrent t;
		t.id = strtolong(params["id"]);
		std::string info_hash = params["info_hash"];
		info_hash = hex_decode(info_hash);
		if (params["freetorrent"] == "0") {
			t.free_torrent = NORMAL;
		} else if (params["freetorrent"] == "1") {
			t.free_torrent = FREE;
		} else {
			t.free_torrent = NEUTRAL;
		}
		t.balance = 0;
		t.completed = 0;
		t.last_selected_seeder = "";
		torrents_list[info_hash] = t;
		std::cout << "Added torrent " << t.id<< ". FL: " << t.free_torrent << " " << params["freetorrent"] << std::endl;
	} else if (params["action"] == "update_torrent") {
		std::string info_hash = params["info_hash"];
		info_hash = hex_decode(info_hash);
		freetype fl;
		if (params["freetorrent"] == "0") {
			fl = NORMAL;
		} else if (params["freetorrent"] == "1") {
			fl = FREE;
		} else {
			fl = NEUTRAL;
		}
		auto torrent_it = torrents_list.find(info_hash);
		if (torrent_it != torrents_list.end()) {
			torrent_it->second.free_torrent = fl;
			std::cout << "Updated torrent " << torrent_it->second.id << " to FL " << fl << std::endl;
		} else {
			std::cout << "Failed to find torrent " << info_hash << " to FL " << fl << std::endl;
		}
	} else if (params["action"] == "update_torrents") {
		// Each decoded infohash is exactly 20 characters long.
		std::string info_hashes = params["info_hashes"];
		info_hashes = hex_decode(info_hashes);
		freetype fl;
		if (params["freetorrent"] == "0") {
			fl = NORMAL;
		} else if (params["freetorrent"] == "1") {
			fl = FREE;
		} else {
			fl = NEUTRAL;
		}
		for (unsigned int pos = 0; pos < info_hashes.length(); pos += 20) {
			std::string info_hash = info_hashes.substr(pos, 20);
			auto torrent_it = torrents_list.find(info_hash);
			if (torrent_it != torrents_list.end()) {
				torrent_it->second.free_torrent = fl;
				std::cout << "Updated torrent " << torrent_it->second.id << " to FL " << fl << std::endl;
			} else {
				std::cout << "Failed to find torrent " << info_hash << " to FL " << fl << std::endl;
			}
		}
	} else if (params["action"] == "add_token") {
		std::string info_hash = hex_decode(params["info_hash"]);
		int user_id = atoi(params["userid"].c_str());
		auto torrent_it = torrents_list.find(info_hash);
		if (torrent_it != torrents_list.end()) {
			torrent_it->second.tokened_users.insert(user_id);
		} else {
			std::cout << "Failed to find torrent to add a token for user " << user_id << std::endl;
		}
	} else if (params["action"] == "remove_token") {
		std::string info_hash = hex_decode(params["info_hash"]);
		int user_id = atoi(params["userid"].c_str());
		auto torrent_it = torrents_list.find(info_hash);
		if (torrent_it != torrents_list.end()) {
			torrent_it->second.tokened_users.erase(user_id);
		} else {
			std::cout << "Failed to find torrent " << info_hash << " to remove token for user " << user_id << std::endl;
		}
	} else if (params["action"] == "delete_torrent") {
		std::string info_hash = params["info_hash"];
		info_hash = hex_decode(info_hash);
		int reason = -1;
		auto reason_it = params.find("reason");
		if (reason_it != params.end()) {
			reason = atoi(params["reason"].c_str());
		}
		auto torrent_it = torrents_list.find(info_hash);
		if (torrent_it != torrents_list.end()) {
			std::cout << "Deleting torrent " << torrent_it->second.id << " for the reason '" << get_del_reason(reason) << "'" << std::endl;
			boost::mutex::scoped_lock lock(del_reasons_lock);
			del_message msg;
			msg.reason = reason;
			msg.time = time(NULL);
			del_reasons[info_hash] = msg;
			torrents_list.erase(torrent_it);
		} else {
			std::cout << "Failed to find torrent " << info_hash << " to delete " << std::endl;
		}
	} else if (params["action"] == "add_user") {
		std::string passkey = params["passkey"];
		unsigned int id = strtolong(params["id"]);
		user u;
		u.id = id;
		u.can_leech = 1;
		if (params["visible"] == "0") {
			u.protect_ip = 1;
		} else {
			u.protect_ip = 0;
		}
		users_list[passkey] = u;
		std::cout << "Added user " << id << std::endl;
	} else if (params["action"] == "remove_user") {
		std::string passkey = params["passkey"];
		users_list.erase(passkey);
		std::cout << "Removed user " << passkey << std::endl;
	} else if (params["action"] == "remove_users") {
		// Each passkey is exactly 32 characters long.
		std::string passkeys = params["passkeys"];
		for (unsigned int pos = 0; pos < passkeys.length(); pos += 32) {
			std::string passkey = passkeys.substr(pos, 32);
			users_list.erase(passkey);
			std::cout << "Removed user " << passkey << std::endl;
		}
	} else if (params["action"] == "update_user") {
		std::string passkey = params["passkey"];
		bool can_leech = true;
		bool protect_ip = false;
		if (params["can_leech"] == "0") {
			can_leech = false;
		}
		if (params["visible"] == "0") {
			protect_ip = true;
		}
		user_list::iterator i = users_list.find(passkey);
		if (i == users_list.end()) {
			std::cout << "No user with passkey " << passkey << " found when attempting to change leeching status!" << std::endl;
		} else {
			users_list[passkey].protect_ip = protect_ip;
			users_list[passkey].can_leech = can_leech;
			std::cout << "Updated user " << passkey << std::endl;
		}
	} else if (params["action"] == "add_whitelist") {
		std::string peer_id = params["peer_id"];
		whitelist.push_back(peer_id);
		std::cout << "Whitelisted " << peer_id << std::endl;
	} else if (params["action"] == "remove_whitelist") {
		std::string peer_id = params["peer_id"];
		for (unsigned int i = 0; i < whitelist.size(); i++) {
			if (whitelist[i].compare(peer_id) == 0) {
				whitelist.erase(whitelist.begin() + i);
				break;
			}
		}
		std::cout << "De-whitelisted " << peer_id << std::endl;
	} else if (params["action"] == "edit_whitelist") {
		std::string new_peer_id = params["new_peer_id"];
		std::string old_peer_id = params["old_peer_id"];
		for (unsigned int i = 0; i < whitelist.size(); i++) {
			if (whitelist[i].compare(old_peer_id) == 0) {
				whitelist.erase(whitelist.begin() + i);
				break;
			}
		}
		whitelist.push_back(new_peer_id);
		std::cout << "Edited whitelist item from " << old_peer_id << " to " << new_peer_id << std::endl;
	} else if (params["action"] == "update_announce_interval") {
		unsigned int interval = strtolong(params["new_announce_interval"]);
		conf->announce_interval = interval;
		std::cout << "Edited announce interval to " << interval << std::endl;
	} else if (params["action"] == "info_torrent") {
		std::string info_hash_hex = params["info_hash"];
		std::string info_hash = hex_decode(info_hash_hex);
		std::cout << "Info for torrent '" << info_hash_hex << "'" << std::endl;
		auto torrent_it = torrents_list.find(info_hash);
		if (torrent_it != torrents_list.end()) {
			std::cout << "Torrent " << torrent_it->second.id
				<< ", freetorrent = " << torrent_it->second.free_torrent << std::endl;
		} else {
			std::cout << "Failed to find torrent " << info_hash_hex << std::endl;
		}
	}
	return "success";
}

void worker::reap_peers() {
	boost::thread thread(&worker::do_reap_peers, this);
	boost::thread t(&worker::do_reap_del_reasons, this);
}

void worker::do_reap_peers() {
	std::cout << "Starting peer reaper" << std::endl;
	cur_time = time(NULL);
	unsigned int reaped = 0;
	torrent_list::iterator i = torrents_list.begin();
	for (; i != torrents_list.end(); i++) {
		peer_list::iterator p = i->second.leechers.begin();
		peer_list::iterator del_p;
		while(p != i->second.leechers.end()) {
			if (p->second.last_announced + conf->peers_timeout < cur_time) {
				del_p = p;
				p++;
				boost::mutex::scoped_lock lock(db->torrent_list_mutex);
				i->second.leechers.erase(del_p);
				reaped++;
			} else {
				p++;
			}
		}
		p = i->second.seeders.begin();
		while(p != i->second.seeders.end()) {
			if (p->second.last_announced + conf->peers_timeout < cur_time) {
				del_p = p;
				p++;
				boost::mutex::scoped_lock lock(db->torrent_list_mutex);
				i->second.seeders.erase(del_p);
				reaped++;
			} else {
				p++;
			}
		}
	}
	std::cout << "Reaped " << reaped << " peers" << std::endl;
}

void worker::do_reap_del_reasons()
{
	std::cout << "Starting del reason reaper" << std::endl;
	time_t max_time = time(NULL) - conf->del_reason_lifetime;
	auto it = del_reasons.begin();
	unsigned int reaped = 0;
	for (; it != del_reasons.end(); ) {
		if (it->second.time <= max_time) {
			auto del_it = it;
			it++;
			boost::mutex::scoped_lock lock(del_reasons_lock);
			del_reasons.erase(del_it);
			reaped++;
			continue;
		}
		it++;
	}
	std::cout << "Reaped " << reaped << " del reasons" << std::endl;
}

std::string worker::get_del_reason(int code)
{
	switch (code) {
		case DUPE:
			return "Dupe";
			break;
		case TRUMP:
			return "Trump";
			break;
		case BAD_FILE_NAMES:
			return "Bad File Names";
			break;
		case BAD_FOLDER_NAMES:
			return "Bad Folder Names";
			break;
		case BAD_TAGS:
			return "Bad Tags";
			break;
		case BAD_FORMAT:
			return "Disallowed Format";
			break;
		case DISCS_MISSING:
			return "Discs Missing";
			break;
		case DISCOGRAPHY:
			return "Discography";
			break;
		case EDITED_LOG:
			return "Edited Log";
			break;
		case INACCURATE_BITRATE:
			return "Inaccurate Bitrate";
			break;
		case LOW_BITRATE:
			return "Low Bitrate";
			break;
		case MUTT_RIP:
			return "Mutt Rip";
			break;
		case BAD_SOURCE:
			return "Disallowed Source";
			break;
		case ENCODE_ERRORS:
			return "Encode Errors";
			break;
		case BANNED:
			return "Specifically Banned";
			break;
		case TRACKS_MISSING:
			return "Tracks Missing";
			break;
		case TRANSCODE:
			return "Transcode";
			break;
		case CASSETTE:
			return "Unapproved Cassette";
			break;
		case UNSPLIT_ALBUM:
			return "Unsplit Album";
			break;
		case USER_COMPILATION:
			return "User Compilation";
			break;
		case WRONG_FORMAT:
			return "Wrong Format";
			break;
		case WRONG_MEDIA:
			return "Wrong Media";
			break;
		case AUDIENCE:
			return "Audience Recording";
			break;
		default:
			return "";
			break;
	}
}

/* Peers should be invisible if they are a leecher without
   download privs or their IP is invalid */
bool worker::peer_is_visible(user *u, peer *p) {
	return (p->left == 0 || u->can_leech) && !p->invalid_ip;
}
