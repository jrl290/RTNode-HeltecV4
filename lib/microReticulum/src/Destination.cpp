#include "Destination.h"

#include "Transport.h"
#include "Interface.h"
#include "Packet.h"
#include "Log.h"

#include <vector>
#include <time.h>
#include <string.h>

using namespace RNS;
using namespace RNS::Type::Destination;
using namespace RNS::Utilities;

Destination::Destination(const Identity& identity, const directions direction, const types type, const char* app_name, const char* aspects) : _object(new Object(identity)) {
	assert(_object);
	MEM("Destination object creating..., this: " + std::to_string((uintptr_t)this) + ", data: " + std::to_string((uintptr_t)_object.get()));

	// Check input values and build name string
	if (strchr(app_name, '.') != nullptr) {
		throw std::invalid_argument("Dots can't be used in app names");
	}
	//TRACE("Destination::Destination: app name: " + std::string(app_name));

	_object->_type = type;
	_object->_direction = direction;

	std::string fullaspects(aspects);
	if (!identity && direction == IN && _object->_type != PLAIN) {
		TRACE("Destination::Destination: identity not provided, creating new one");
		_object->_identity = Identity();
		// CBA TODO determine why identity.hexhash is added both here and by expand_name called below
		fullaspects += "." + _object->_identity.hexhash();
	}
	//TRACE("Destination::Destination: full aspects: " + fullaspects);

	if (_object->_identity && _object->_type == PLAIN) {
		throw std::invalid_argument("Selected destination type PLAIN cannot hold an identity");
	}

	_object->_name = expand_name(_object->_identity, app_name, fullaspects.c_str());
	//TRACE("Destination::Destination: name: " + _object->_name);

	// Generate the destination address hash
	//TRACE("Destination::Destination: creating hash...");
	_object->_hash = hash(_object->_identity, app_name, fullaspects.c_str());
	_object->_hexhash = _object->_hash.toHex();
	TRACE("Destination::Destination: hash: " + _object->_hash.toHex());
	//TRACE("Destination::Destination: creating name hash...");
	_object->_name_hash = name_hash(app_name, aspects);
	//TRACE("Destination::Destination: name hash: " + _object->_name_hash.toHex());

	//TRACE("Destination::Destination: calling register_destination");
	Transport::register_destination(*this);

	MEM("Destination object created, this: " + std::to_string((uintptr_t)this) + ", data: " + std::to_string((uintptr_t)_object.get()));
}

Destination::Destination(const Identity& identity, const Type::Destination::directions direction, const Type::Destination::types type, const Bytes& hash) : _object(new Object(identity)) {
	assert(_object);
	MEM("Destination object creating..., this: " + std::to_string((uintptr_t)this) + ", data: " + std::to_string((uintptr_t)_object.get()));

	_object->_type = type;
	_object->_direction = direction;

	if (_object->_identity && _object->_type == PLAIN) {
		throw std::invalid_argument("Selected destination type PLAIN cannot hold an identity");
	}

	_object->_hash = hash;
	_object->_hexhash = _object->_hash.toHex();
	TRACE("Destination::Destination: hash: " + _object->_hash.toHex());
	//TRACE("Destination::Destination: creating name hash...");
	_object->_name_hash = name_hash("unknown", "unknown");
	//TRACE("Destination::Destination: name hash: " + _object->_name_hash.toHex());

	//TRACE("Destination::Destination: calling register_destination");
	Transport::register_destination(*this);

	MEM("Destination object created, this: " + std::to_string((uintptr_t)this) + ", data: " + std::to_string((uintptr_t)_object.get()));
}

/*virtual*/ Destination::~Destination() {
	MEM("Destination object destroyed, this: " + std::to_string((uintptr_t)this) + ", data: " + std::to_string((uintptr_t)_object.get()));
	if (_object && _object.use_count() == 1) {
		MEM("Destination object has last data reference");

		// CBA Can't call deregister_destination here because it's possible (likely even) that Destination
		//  is being destructed from that same collection which will result in a llop and memory errors.
		//TRACE("Destination::~Destination: calling deregister_destination");
		//Transport::deregister_destination(*this);
	}
}

/*
:returns: A destination name in adressable hash form, for an app_name and a number of aspects.
*/
/*static*/ Bytes Destination::hash(const Identity& identity, const char* app_name, const char* aspects) {
	Bytes addr_hash_material = name_hash(app_name, aspects);
	if (identity) {
		addr_hash_material << identity.hash();
	}

	// CBA TODO valid alternative?
	//return Identity::full_hash(addr_hash_material).left(Type::Reticulum::TRUNCATED_HASHLENGTH/8);
	return Identity::truncated_hash(addr_hash_material);
}

/*
:returns: A name in hash form, for an app_name and a number of aspects.
*/
/*static*/ Bytes Destination::name_hash(const char* app_name, const char* aspects) {
	return Identity::full_hash(expand_name({Type::NONE}, app_name, aspects)).left(Type::Identity::NAME_HASH_LENGTH/8);
}

/*
:returns: A tuple containing the app name and a list of aspects, for a full-name string.
*/
/*static*/ std::vector<std::string> Destination::app_and_aspects_from_name(const char* full_name) {
	std::vector<std::string> components;
	std::string name(full_name);
	std::size_t pos = name.find('.');
	components.push_back(name.substr(0, pos));
	if (pos != std::string::npos) {
		components.push_back(name.substr(pos+1));
	}
	return components;
}

/*
:returns: A destination name in adressable hash form, for a full name string and Identity instance.
*/
/*static*/ Bytes Destination::hash_from_name_and_identity(const char* full_name, const Identity& identity) {
	std::vector<std::string> components = app_and_aspects_from_name(full_name);
	if (components.size() == 0) {
		return {Bytes::NONE};
	}
	if (components.size() == 1) {
		return hash(identity, components[0].c_str(), "");
	}
	return hash(identity, components[0].c_str(), components[1].c_str());
}

/*
:returns: A string containing the full human-readable name of the destination, for an app_name and a number of aspects.
*/
/*static*/ std::string Destination::expand_name(const Identity& identity, const char* app_name, const char* aspects) {

	if (strchr(app_name, '.') != nullptr) {
		throw std::invalid_argument("Dots can't be used in app names");
	}

	std::string name(app_name);

	if (aspects != nullptr) {
		name += std::string(".") + aspects;
	}

	if (identity) {
		name += "." + identity.hexhash();
	}

	return name;
}

/*
Creates an announce packet for this destination and broadcasts it on all
relevant interfaces. Application specific data can be added to the announce.

:param app_data: *bytes* containing the app_data.
:param path_response: Internal flag used by :ref:`RNS.Transport<api-transport>`. Ignore.
*/
//Packet Destination::announce(const Bytes& app_data /*= {}*/, bool path_response /*= false*/, const Interface& attached_interface /*= {Type::NONE}*/, const Bytes& tag /*= {}*/, bool send /*= true*/) {
Packet Destination::announce(const Bytes& app_data, bool path_response, const Interface& attached_interface, const Bytes& tag /*= {}*/, bool send /*= true*/) {
	assert(_object);
	TRACE("Destination::announce: announcing destination...");

	if (_object->_type != SINGLE) {
		throw std::invalid_argument("Only SINGLE destination types can be announced");
	}

	if (_object->_direction != IN) {
		throw std::invalid_argument("Only IN destination types can be announced");
	}

	double now = OS::time();
    auto it = _object->_path_responses.begin();
    while (it != _object->_path_responses.end()) {
		// vector
		//Response& entry = *it;
		// map
		PathResponse& entry = (*it).second;
		if (now > (entry.first + PR_TAG_WINDOW)) {
			it = _object->_path_responses.erase(it);
		}
		else {
			++it;
		}
	}

	Bytes announce_data;

/*
	// CBA TEST
	TRACE("Destination::announce: performing path test...");
	TRACE("Destination::announce: inserting path...");
	_object->_path_responses.insert({Bytes("foo_tag"), {0, Bytes("this is foo tag")}});
	TRACE("Destination::announce: inserting path...");
	_object->_path_responses.insert({Bytes("test_tag"), {0, Bytes("this is test tag")}});
	if (path_response) {
		TRACE("Destination::announce: path_response is true");
	}
	if (!tag.empty()) {
		TRACE("Destination::announce: tag is specified");
		std::string tagstr((const char*)tag.data(), tag.size());
		DEBUG(std::string("Destination::announce: tag: ") + tagstr);
		DEBUG(std::string("Destination::announce: tag len: ") + std::to_string(tag.size()));
		TRACE("Destination::announce: searching for tag...");
		if (_object->_path_responses.find(tag) != _object->_path_responses.end()) {
			TRACE("Destination::announce: found tag in _path_responses");
			DEBUG(std::string("Destination::announce: data: ") +_object->_path_responses[tag].second.toString());
		}
		else {
			TRACE("Destination::announce: tag not found in _path_responses");
		}
	}
	TRACE("Destination::announce: path test finished");
*/

	if (path_response && !tag.empty() && _object->_path_responses.find(tag) != _object->_path_responses.end()) {
		// This code is currently not used, since Transport will block duplicate
		// path requests based on tags. When multi-path support is implemented in
		// Transport, this will allow Transport to detect redundant paths to the
		// same destination, and select the best one based on chosen criteria,
		// since it will be able to detect that a single emitted announce was
		// received via multiple paths. The difference in reception time will
		// potentially also be useful in determining characteristics of the
		// multiple available paths, and to choose the best one.
		announce_data << _object->_path_responses[tag].second;
	}
	else {
		Bytes destination_hash = _object->_hash;
		Bytes random_hash = Cryptography::random(5);
		uint64_t emitted = (uint64_t)OS::time();
		for (int shift = 32; shift >= 0; shift -= 8) {
			random_hash << (uint8_t)((emitted >> shift) & 0xFF);
		}

		Bytes new_app_data(app_data);
        if (new_app_data.empty() && !_object->_default_app_data.empty()) {
			new_app_data = _object->_default_app_data;
		}

		Bytes signed_data;
		//TRACE("Destination::announce: hash:         " + _object->_hash.toHex());
		//TRACE("Destination::announce: public key:   " + _object->_identity.get_public_key().toHex());
		//TRACE("Destination::announce: name hash:    " + _object->_name_hash.toHex());
		//TRACE("Destination::announce: random hash:  " + random_hash.toHex());
		//TRACE("Destination::announce: app data:     " + new_app_data.toHex());
		//TRACE("Destination::announce: app data text:" + new_app_data.toString());
		signed_data << _object->_hash << _object->_identity.get_public_key() << _object->_name_hash << random_hash;
		if (new_app_data) {
			signed_data << new_app_data;
		}
		//TRACE("Destination::announce: signed data:  " + signed_data.toHex());

		Bytes signature(_object->_identity.sign(signed_data));
		//TRACE("Destination::announce: signature:    " + signature.toHex());

		announce_data << _object->_identity.get_public_key() << _object->_name_hash << random_hash << signature;

		if (new_app_data) {
			announce_data << new_app_data;
		}

		// CBA ACCUMULATES
		_object->_path_responses.insert({tag, {OS::time(), announce_data}});
	}
	//TRACE("Destination::announce: announce_data:" + announce_data.toHex());

	Type::Packet::context_types announce_context = Type::Packet::CONTEXT_NONE;
	if (path_response) {
		announce_context = Type::Packet::PATH_RESPONSE;
	}

	//TRACE("Destination::announce: creating announce packet...");
	//Packet announce_packet(*this, announce_data, Type::Packet::ANNOUNCE, announce_context, Type::Transport::BROADCAST, Type::Packet::HEADER_1, nullptr, attached_interface);
	Packet announce_packet(*this, attached_interface, announce_data, Type::Packet::ANNOUNCE, announce_context, Type::Transport::BROADCAST, Type::Packet::HEADER_1);

	if (send) {
		TRACE("Destination::announce: sending announce packet...");
		announce_packet.send();
		return {Type::NONE};
	}
	else {
		return announce_packet;
	}
}

Packet Destination::announce(const Bytes& app_data /*= {}*/, bool path_response /*= false*/) {
	return announce(app_data, path_response, {Type::NONE});
}


/*
Registers a request handler.

:param path: The path for the request handler to be registered.
:param response_generator: A function with the signature *response_generator(path, data, request_id, link_id, remote_identity, requested_at)* returning the response data, or an empty Bytes to send no response.
:param allow: One of ``ALLOW_NONE``, ``ALLOW_ALL`` or ``ALLOW_LIST``. If ``ALLOW_LIST`` is set, the request is only handled if the link peer identified itself with an identity hash contained in *allowed_list*.
:param allowed_list: A set of identity hashes.
:returns: True if the handler was registered, false if an argument was invalid.
*/
bool Destination::register_request_handler(const Bytes& path, RequestHandler::response_generator response_generator, Type::Destination::request_policies allow /*= Type::Destination::ALLOW_NONE*/, const std::set<Bytes>& allowed_list /*= {}*/) {
	assert(_object);
	if (!path) {
		ERROR("Invalid path specified for request handler");
		return false;
	}
	if (response_generator == nullptr) {
		ERROR("Invalid response generator specified for request handler");
		return false;
	}
	if (allow != Type::Destination::ALLOW_NONE && allow != Type::Destination::ALLOW_ALL && allow != Type::Destination::ALLOW_LIST) {
		ERROR("Invalid request policy specified for request handler");
		return false;
	}
	const Bytes path_hash(Identity::truncated_hash(path));
	RequestHandler request_handler;
	request_handler._path = path;
	request_handler._response_generator = response_generator;
	request_handler._allow = allow;
	request_handler._allowed_list = allowed_list;
	// std::map::insert() does not replace an existing key, so erase first
	_object->_request_handlers.erase(path_hash);
	_object->_request_handlers.insert({path_hash, request_handler});
	return true;
}

/*
Deregisters a request handler.

:param path: The path for the request handler to be deregistered.
:returns: True if the handler was deregistered, otherwise False.
*/
bool Destination::deregister_request_handler(const Bytes& path) {
	assert(_object);
	const Bytes path_hash(Identity::truncated_hash(path));
	return _object->_request_handlers.erase(path_hash) > 0;
}

void Destination::receive(const Packet& packet) {
	assert(_object);
	if (packet.packet_type() == Type::Packet::LINKREQUEST) {
		Bytes plaintext(packet.data());
		incoming_link_request(plaintext, packet);
	}
	else {
		// CBA TODO Why isn't the Packet decrypting itself?
		Bytes plaintext(decrypt(packet.data()));
		//TRACE("Destination::receive: decrypted data: " + plaintext.toHex());
		if (plaintext) {
			if (packet.packet_type() == Type::Packet::DATA) {
				if (_object->_callbacks._packet) {
					try {
						_object->_callbacks._packet(plaintext, packet);
					}
					catch (std::exception& e) {
						DEBUG("Error while executing receive callback from " + toString() + ". The contained exception was: " + e.what());
					}
				}
			}
		}
	}
}

void Destination::incoming_link_request(const Bytes& data, const Packet& packet) {
	assert(_object);
	if (_object->_accept_link_requests) {
TRACE("***** Accepting link request");
		RNS::Link link = Link::validate_request(*this, data, packet);
		if (link) {
			_object->_links.insert(link);
		}
	}
}

/*
Encrypts information for ``RNS.Destination.SINGLE`` or ``RNS.Destination.GROUP`` type destination.

:param plaintext: A *bytes-like* containing the plaintext to be encrypted.
:raises: ``ValueError`` if destination does not hold a necessary key for encryption.
*/
/*virtual*/ const Bytes Destination::encrypt(const Bytes& data) {
	assert(_object);
	TRACE("Destination::encrypt: encrypting data...");

	if (_object->_type == PLAIN) {
		return data;
	}

	if (_object->_type == SINGLE && _object->_identity) {
		return _object->_identity.encrypt(data);
	}

// TODO
/*
	if (_object->_type == GROUP {
		if hasattr(self, "prv") and self.prv != None:
			try:
				return self.prv.encrypt(plaintext)
			except Exception as e:
				RNS.log("The GROUP destination could not encrypt data", RNS.LOG_ERROR)
				RNS.log("The contained exception was: "+str(e), RNS.LOG_ERROR)
		else:
			raise ValueError("No private key held by GROUP destination. Did you create or load one?")
	}
*/

	// CBA Reference implementation does not handle this default case
	// CBA TODO Determine of returning plaintext is appropriate here (for now it's necessary for PROOF packets)
	return data;
}

/*
Decrypts information for ``RNS.Destination.SINGLE`` or ``RNS.Destination.GROUP`` type destination.

:param ciphertext: *Bytes* containing the ciphertext to be decrypted.
:raises: ``ValueError`` if destination does not hold a necessary key for decryption.
*/
/*virtual*/ const Bytes Destination::decrypt(const Bytes& data) {
	assert(_object);
	TRACE("Destination::decrypt: decrypting data...");

	if (_object->_type == PLAIN) {
		return data;
	}

	if (_object->_type == SINGLE && _object->_identity) {
		return _object->_identity.decrypt(data);
	}

/*
	if (_object->_type == GROUP) {
		if hasattr(self, "prv") and self.prv != None:
			try:
				return self.prv.decrypt(ciphertext)
			except Exception as e:
				RNS.log("The GROUP destination could not decrypt data", RNS.LOG_ERROR)
				RNS.log("The contained exception was: "+str(e), RNS.LOG_ERROR)
		else:
			raise ValueError("No private key held by GROUP destination. Did you create or load one?")
	}
*/
	// MOCK
	return {Bytes::NONE};
}

/*
Signs information for ``RNS.Destination.SINGLE`` type destination.

:param message: *Bytes* containing the message to be signed.
:returns: A *bytes-like* containing the message signature, or *None* if the destination could not sign the message.
*/
/*virtual*/ const Bytes Destination::sign(const Bytes& message) {
	assert(_object);
	if (_object->_type == SINGLE && _object->_identity) {
		return _object->_identity.sign(message);
	}
	return {Bytes::NONE};
}

bool Destination::has_link(const Link& link) {
	assert(_object);
	return (_object->_links.count(link) > 0);
}

void Destination::remove_link(const Link& link) {
	assert(_object);
	_object->_links.erase(link);
}
