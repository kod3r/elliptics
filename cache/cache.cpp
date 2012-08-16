/*
 * 2012+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <iostream>
#include <vector>

#include <boost/unordered_map.hpp>
#include <boost/shared_array.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include <boost/thread.hpp>

#include "config.h"

#include "../library/elliptics.h"

#include "elliptics/packet.h"
#include "elliptics/interface.h"

namespace ioremap { namespace cache {

struct key_t {
	key_t(const unsigned char *id) {
		memcpy(this->id, id, DNET_ID_SIZE);
	}

	unsigned char id[DNET_ID_SIZE];
};

size_t hash(const unsigned char *id) {
	size_t num = DNET_ID_SIZE / sizeof(size_t);

	size_t *ptr = (size_t *)id;
	size_t hash = 0x883eaf5a;
	for (size_t i = 0; i < num; ++i)
		hash ^= ptr[i];

	return hash;
}

struct hash_t {
	std::size_t operator()(const key_t &key) const {
		return ioremap::cache::hash(key.id);
	}
};

struct equal_to {
	bool operator() (const key_t &x, const key_t &y) const {
		return memcmp(x.id, y.id, DNET_ID_SIZE) == 0;
	}
};

class raw_data_t {
	public:
		raw_data_t(const char *data, size_t size) {
			m_data.reset(new char[size]);
			memcpy(m_data.get(), data, size);
			m_size = size;
		}

		~raw_data_t() {
		}

		const char *data(void) const {
			return m_data.get();
		}

		size_t size(void) const {
			return m_size;
		}

	private:
		boost::shared_array<char> m_data;
		size_t m_size;
};

typedef boost::shared_ptr<raw_data_t> data_t;
typedef boost::unordered_map<key_t, data_t, hash_t, equal_to> hmap_t;
#if 0
/*
 * we can not create vector of mutexes since they are not movable
 */
class cache_t {
	public:
		cache_t(int num) {
			m_locks.resize(std::max(num, 1));
		}
		~cache_t();

		void write(const unsigned char *id, const char *data, size_t size) {
			int idx = hash(id) % m_locks.size();
			boost::mutex::scoped_lock guard(m_locks[idx]);

			m_hmap[id] = boost::make_shared<raw_data_t>(data, size);
		}

		data_t &read(const unsigned char *id) {
			int idx = hash(id) % m_locks.size();
			boost::mutex::scoped_lock guard(m_locks[idx]);

			hmap_t::iterator it = m_hmap.find(id);
			if (it == m_hmap.end())
				throw std::runtime_error("no record");

			return it->second;
		}

	private:
		hmap_t m_hmap;
		std::vector<boost::mutex> m_locks;
};
#else
class cache_t {
	public:
		void write(const unsigned char *id, const char *data, size_t size) {
			boost::mutex::scoped_lock guard(m_lock);
			m_hmap[id] = boost::make_shared<raw_data_t>(data, size);
		}

		data_t &read(const unsigned char *id) {
			boost::mutex::scoped_lock guard(m_lock);

			hmap_t::iterator it = m_hmap.find(id);
			if (it == m_hmap.end())
				throw std::runtime_error("no record");

			return it->second;
		}

		void remove(const unsigned char *id) {
			boost::mutex::scoped_lock guard(m_lock);
			m_hmap.erase(id);
		}

	private:
		hmap_t m_hmap;
		boost::mutex m_lock;
};
#endif

}}

using namespace ioremap::cache;

int dnet_cmd_cache_io(struct dnet_net_state *st, struct dnet_cmd *cmd, char *data)
{
	struct dnet_node *n = st->n;
	int err = -ENOTSUP;

	if (!n->cache)
		return -ENOTSUP;

	cache_t *cache = (cache_t *)n->cache;

	try {
		struct dnet_io_attr *io = (struct dnet_io_attr *)data;
		data_t d;

		data += sizeof(struct dnet_io_attr);

		switch (cmd->cmd) {
			case DNET_CMD_WRITE:
				cache->write(io->id, data, io->size);
				err = 0;
				break;
			case DNET_CMD_READ:
				d = cache->read(io->id);
				if (io->offset + io->size > d->size()) {
					dnet_log_raw(n, DNET_LOG_ERROR, "%s: %s cache: invalid offset/size: "
							"offset: %llu, size: %llu, cached-size: %zd\n",
							dnet_dump_id(&cmd->id), dnet_cmd_string(cmd->cmd),
							(unsigned long long)io->offset, (unsigned long long)io->size,
							d->size());
					err = -EINVAL;
					break;
				}

				io->size = d->size();
				err = dnet_send_read_data(st, cmd, io, (char *)d->data() + io->offset, -1, io->offset, 0);
				break;
			case DNET_CMD_DEL:
				cache->remove(cmd->id.id);
				break;
		}
	} catch (const std::exception &e) {
		dnet_log_raw(n, DNET_LOG_ERROR, "%s: %s cache operation failed: %s\n",
				dnet_dump_id(&cmd->id), dnet_cmd_string(cmd->cmd), e.what());
		err = -ENOENT;
	}

	return err;
}

int dnet_cache_init(struct dnet_node *n)
{
	try {
		n->cache = (void *)(new cache_t);
	} catch (const std::exception &e) {
		dnet_log_raw(n, DNET_LOG_ERROR, "Could not create cache: %s\n", e.what());
		return -ENOMEM;
	}

	return 0;
}

void dnet_cache_cleanup(struct dnet_node *n)
{
	if (n->cache)
		delete (cache_t *)n->cache;
}