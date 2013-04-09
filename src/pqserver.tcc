// -*- mode: c++ -*-
#include <unistd.h>
#include <set>
#include <vector>
#include "pqserver.hh"
#include "pqjoin.hh"
#include "json.hh"
#include "error.hh"
#include <sys/resource.h>

namespace pq {

const Datum Datum::empty_datum{Str()};
const Datum Datum::max_datum(Str("\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"));
Table Table::empty_table{Str(), nullptr, nullptr};
const char Datum::table_marker[] = "TABLE MARKER";

void Table::iterator::fix() {
 retry:
    if (it_ == table_->store_.end()) {
        if (table_->parent_) {
            it_ = table_->parent_->store_.iterator_to(*table_);
            table_ = table_->parent_;
            ++it_;
            goto retry;
        }
    } else if (it_->is_table()) {
        table_ = &it_->table();
        it_ = table_->store_.begin();
        goto retry;
    }
}

Table::Table(Str name, Table* parent, Server* server)
    : Datum(name, String::make_stable(Datum::table_marker)),
      triecut_(0), njoins_(0), flush_at_(0), all_pull_(true),
      server_{server}, parent_{parent},
      ninsert_(0), nmodify_(0), nmodify_nohint_(0), nerase_(0), nvalidate_(0) {
}

Table::~Table() {
    while (SourceRange* r = source_ranges_.unlink_leftmost_without_rebalance()) {
        r->clear_without_deref();
        delete r;
    }
    while (JoinRange* r = join_ranges_.unlink_leftmost_without_rebalance())
        delete r;
    // delete store last since join_ranges_ have refs to Datums
    while (Datum* d = store_.unlink_leftmost_without_rebalance()) {
        if (d->is_table())
            delete &d->table();
        else
            delete d;
    }
}

Table* Table::next_table_for(Str key) {
    if (subtable_hashable()) {
        if (Table** tp = subtables_.get_pointer(subtable_hash_for(key)))
            return *tp;
    } else {
        auto it = store_.lower_bound(key.prefix(triecut_), DatumCompare());
        if (it != store_.end() && it->key() == key.prefix(triecut_))
            return &it->table();
    }
    return this;
}

Table* Table::make_next_table_for(Str key) {
    bool can_hash = subtable_hashable();
    if (can_hash) {
        if (Table* t = subtables_[subtable_hash_for(key)])
            return t;
    }

    auto it = store_.lower_bound(key.prefix(triecut_), DatumCompare());
    if (it != store_.end() && it->key() == key.prefix(triecut_))
        return &it->table();

    Table* t = new Table(key.prefix(triecut_), this, server_);
    t->all_pull_ = false;
    store_.insert_before(it, *t);

    if (can_hash)
        subtables_[subtable_hash_for(key)] = t;
    return t;
}

auto Table::lower_bound(Str key) -> iterator {
    Table* tbl = this;
    int len;
 retry:
    len = tbl->triecut_ ? tbl->triecut_ : key.length();
    auto it = tbl->store_.lower_bound(key.prefix(len), DatumCompare());
    if (len == tbl->triecut_ && it != tbl->store_.end() && it->key() == key.prefix(len)) {
        assert(it->is_table());
        tbl = static_cast<Table*>(it.operator->());
        goto retry;
    }
    return iterator(tbl, it);
}

size_t Table::count(Str key) const {
    const Table* tbl = this;
    int len;
 retry:
    len = tbl->triecut_ ? tbl->triecut_ : key.length();
    auto it = tbl->store_.lower_bound(key.prefix(len), DatumCompare());
    if (it != tbl->store_.end() && it->key() == key.prefix(len)) {
        if (len == tbl->triecut_) {
            assert(it->is_table());
            tbl = static_cast<const Table*>(it.operator->());
            goto retry;
        } else
            return 1;
    }
    return 0;
}

size_t Table::size() const {
    size_t x = store_.size();
    if (triecut_)
        for (auto& d : store_)
            if (d.is_table())
                x += static_cast<const Table&>(d).size();
    return x;
}

void Table::add_source(SourceRange* r) {
    for (auto it = source_ranges_.begin_contains(r->interval());
	 it != source_ranges_.end(); ++it)
	if (it->join() == r->join() && it->joinpos() == r->joinpos()) {
	    // XXX may copy too much. This will not actually cause visible
	    // bugs I think?, but will grow the store
	    it->take_results(*r);
            delete r;
	    return;
	}
    source_ranges_.insert(*r);
}

void Table::remove_source(Str first, Str last, SinkRange* sink, Str context) {
    for (auto it = source_ranges_.begin_overlaps(first, last);
	 it != source_ranges_.end(); ) {
        SourceRange* source = it.operator->();
        ++it;
	if (source->join() == sink->join())
            source->remove_sink(sink, context);
    }
}

void Table::add_join(Str first, Str last, Join* join, ErrorHandler* errh) {
    FileErrorHandler xerrh(stderr);
    errh = errh ? errh : &xerrh;

    // check for redundant join
    for (auto it = join_ranges_.begin_overlaps(first, last);
         it != join_ranges_.end(); ++it)
        if (it->join()->same_structure(*join)) {
            errh->error("join on [%p{Str}, %p{Str}) has same structure as overlapping join\n(new join ignored)", &first, &last);
            return;
        }

    join_ranges_.insert(*new JoinRange(first, last, join));
    if (join->maintained() || join->staleness())
        all_pull_ = false;
    ++njoins_;

    // if this is a distributed deployment, make invalid sink ranges
    // for all remote partitions
    if (server_->partitioner()) {
        std::vector<keyrange> parts;
        server_->partitioner()->analyze(first, last, 0, parts);
        parts.push_back(keyrange(last, 0));

        for (auto& r : parts) {
            if (server_->is_remote(r.owner)) {
                std::cerr << "remote keyrange " << r.key << std::endl;
            }
        }
    }
}

void Server::add_join(Str first, Str last, Join* join, ErrorHandler* errh) {
    join->attach(*this);
    Str tname = table_name(first, last);
    assert(tname);
    make_table(tname).add_join(first, last, join, errh);

    // handle cuts: push only
    if (join->maintained())
        for (int i = 0; i != join->npattern(); ++i) {
            Table& t = make_table(join->pattern(i).table_name());
            int tc = join->pattern_subtable_length(i);
            if (t.triecut_ == 0 && t.store_.empty() && tc)
                t.triecut_ = tc;
        }
}

auto Table::insert(Table& t) -> local_iterator {
    assert(!triecut_ || t.name().length() < triecut_);
    store_type::insert_commit_data cd;
    auto p = store_.insert_check(t.name(), DatumCompare(), cd);
    assert(p.second);
    return store_.insert_commit(t, cd);
}

void Table::insert(Str key, String value) {
    assert(!triecut_ || key.length() < triecut_);
    store_type::insert_commit_data cd;
    auto p = store_.insert_check(key, DatumCompare(), cd);
    Datum* d;
    if (p.second) {
	d = new Datum(key, value);
        value = String();
	store_.insert_commit(*d, cd);
    } else {
	d = p.first.operator->();
        d->value().swap(value);
    }
    notify(d, value, p.second ? SourceRange::notify_insert : SourceRange::notify_update);
    ++ninsert_;
    all_pull_ = false;
}

void Table::erase(Str key) {
    assert(!triecut_ || key.length() < triecut_);
    auto it = store_.find(key, DatumCompare());
    if (it != store_.end())
        erase(iterator(this, it));
    ++nerase_;
}

std::pair<ServerStore::iterator, bool> Table::prepare_modify(Str key, const SinkRange* sink, ServerStore::insert_commit_data& cd) {
    assert(name() && sink);
    assert(!triecut_ || key.length() < triecut_);
    std::pair<ServerStore::iterator, bool> p;
    Datum* hint = sink->hint();
    if (!hint || !hint->valid()) {
        ++nmodify_nohint_;
        p = store_.insert_check(key, DatumCompare(), cd);
    } else {
        p.first = store_.iterator_to(*hint);
        if (hint->key() == key)
            p.second = false;
        else if (hint == store_.rbegin().operator->())
            p = store_.insert_check(store_.end(), key, DatumCompare(), cd);
        else {
            ++p.first;
            p = store_.insert_check(p.first, key, DatumCompare(), cd);
        }
    }
    return p;
}

void Table::finish_modify(std::pair<ServerStore::iterator, bool> p,
                          const store_type::insert_commit_data& cd,
                          Datum* d, Str key, const SinkRange* sink,
                          String value) {
    SourceRange::notify_type n = SourceRange::notify_update;
    if (!is_marker(value)) {
        if (p.second) {
            d = new Datum(key, sink);
            sink->add_datum(d);
            p.first = store_.insert_commit(*d, cd);
            n = SourceRange::notify_insert;
        }
    } else if (is_erase_marker(value)) {
        if (!p.second) {
            p.first = store_.erase(p.first);
            n = SourceRange::notify_erase;
        } else
            goto done;
    } else if (is_invalidate_marker(value)) {
        invalidate_dependents(d->key());
        const_cast<SinkRange*>(sink)->add_invalidate(key);
        goto done;
    } else
        goto done;

    d->value().swap(value);
    notify(d, value, n);
    if (n == SourceRange::notify_erase)
        d->invalidate();

 done:
    sink->update_hint(store_, p.first);
    ++nmodify_;
}

auto Table::validate(Str first, Str last, uint64_t now) -> iterator {
    Table* t = this;
    while (t->parent_->triecut_)
        t = t->parent_;

    if (t->njoins_ != 0) {
        if (t->njoins_ == 1) {
            auto it = store_.lower_bound(first, DatumCompare());
            auto itx = it;
            if ((itx == store_.end() || itx->key() >= last) && itx != store_.begin())
                --itx;
            if (itx != store_.end() && itx->key() < last && itx->owner()
                && itx->owner()->valid() && !itx->owner()->has_expired(now)
                && itx->owner()->ibegin() <= first && last <= itx->owner()->iend()
                && !itx->owner()->need_update())
                return iterator(this, it);
        }
        for (auto it = t->join_ranges_.begin_overlaps(first, last);
             it != t->join_ranges_.end(); ++it)
            it->validate(first, last, *server_, now);
    }
    return lower_bound(first);
}

tamed void Table::prepare_validate(Str key, uint64_t now, tamer::event<> done) {
    LocalStr<24> next_key;
    next_key.assign_uninitialized(key.length() + 1);
    memcpy(next_key.mutable_data(), key.data(), key.length());
    next_key.mutable_data()[key.length()] = 0;
    prepare_validate(key, next_key, now, done);
}

tamed void Table::prepare_validate(Str first, Str last, uint64_t now,
                                   tamer::event<> done) {
    done();
}

void Table::notify(Datum* d, const String& old_value, SourceRange::notify_type notifier) {
    Str key(d->key());
    Table* t = &table_for(key);
 retry:
    for (auto it = t->source_ranges_.begin_contains(key);
         it != t->source_ranges_.end(); ) {
        // SourceRange::notify() might remove the SourceRange from the tree
        SourceRange* source = it.operator->();
        ++it;
        if (source->check_match(key))
            source->notify(d, old_value, notifier);
    }
    if ((t = t->parent_) && t->triecut_)
        goto retry;
}

void Table::invalidate_dependents(Str key) {
    Table* t = &table_for(key);
 retry:
    for (auto it = t->source_ranges_.begin_contains(key);
         it != t->source_ranges_.end(); ) {
        // simple, but obviously we could do better
        SourceRange* source = it.operator->();
        ++it;
        source->invalidate();
    }
    if ((t = t->parent_) && t->triecut_)
        goto retry;
}

inline void Table::invalidate_dependents_local(Str first, Str last) {
    for (auto it = source_ranges_.begin_overlaps(first, last);
         it != source_ranges_.end(); ) {
        // simple, but obviously we could do better
        SourceRange* source = it.operator->();
        ++it;
        source->invalidate();
    }
}

void Table::invalidate_dependents_down(Str first, Str last) {
    for (auto it = store_.lower_bound(first.prefix(triecut_), DatumCompare());
         it != store_.end() && it->key() < last;
         ++it)
        if (it->is_table()) {
            it->table().invalidate_dependents_local(first, last);
            if (it->table().triecut_)
                it->table().invalidate_dependents_down(first, last);
        }
}

void Table::invalidate_dependents(Str first, Str last) {
    Table* t = &table_for(first, last);
    if (triecut_)
        t->invalidate_dependents_down(first, last);
 retry:
    t->invalidate_dependents_local(first, last);
    if ((t = t->parent_) && t->triecut_)
        goto retry;
}

bool Table::hard_flush_for_pull(uint64_t now) {
    while (Datum* d = store_.unlink_leftmost_without_rebalance()) {
        assert(!d->is_table());
        invalidate_dependents(d->key());
        d->invalidate();
    }
    flush_at_ = now;
    return true;
}

Server::Server()
    : supertable_(Str(), nullptr, this),
      last_validate_at_(0), validate_time_(0), insert_time_(0),
      part_(nullptr), hosts_(nullptr), me_(nullptr) {
}

auto Server::create_table(Str tname) -> Table::local_iterator {
    assert(tname);
    Table* t = new Table(tname, &supertable_, this);
    return supertable_.insert(*t);
}

size_t Server::validate_count(Str first, Str last) {
    struct timeval tv[2];
    gettimeofday(&tv[0], NULL);

    Table& t = make_table_for(first, last);
    auto it = t.validate(first, last, next_validate_at());
    auto itend = t.end();
    size_t n = 0;
    for (; it != itend && it->key() < last; ++it)
        ++n;

    gettimeofday(&tv[1], NULL);
    validate_time_ += to_real(tv[1] - tv[0]);
    return n;
}

tamed void Server::prepare_validate(Str key, tamer::event<> done) {
    tvars {
        struct timeval tv[2];
    }

    gettimeofday(&tv[0], NULL);
    twait {
        make_table_for(key).prepare_validate(key,
                                             next_validate_at(),
                                             make_event());
    }

    gettimeofday(&tv[1], NULL);
    prevalidate_time_ += to_real(tv[1] - tv[0]);
    done();
}

tamed void Server::prepare_validate(Str first, Str last, tamer::event<> done) {
    tvars {
        struct timeval tv[2];
    }

    gettimeofday(&tv[0], NULL);
    twait {
        make_table_for(first, last).prepare_validate(first, last,
                                                     next_validate_at(),
                                                     make_event());
    }

    gettimeofday(&tv[1], NULL);
    prevalidate_time_ += to_real(tv[1] - tv[0]);
    done();
}

void Table::add_stats(Json& j) const {
    j["ninsert"] += ninsert_;
    j["nmodify"] += nmodify_;
    j["nmodify_nohint"] += nmodify_nohint_;
    j["nerase"] += nerase_;
    j["store_size"] += store_.size();
    j["source_ranges_size"] += source_ranges_.size();
    for (auto& jr : join_ranges_)
        j["sink_ranges_size"] += jr.valid_ranges_size();
    j["nvalidate"] += nvalidate_;

    if (triecut_)
        for (auto& d : store_)
            if (d.is_table()) {
                j["nsubtables"]++;
                j["store_size"]--;
                d.table().add_stats(j);
            }
}

Json Server::stats() const {
    size_t store_size = 0, source_ranges_size = 0, join_ranges_size = 0,
        sink_ranges_size = 0;
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);

    Json tables = Json::make_array();
    for (auto it = supertable_.lbegin(); it != supertable_.lend(); ++it) {
        assert(it->is_table());
        Table& t = it->table();
        Json j = Json().set("name", t.name());
        t.add_stats(j);
        for (auto it = j.obegin(); it != j.oend(); )
            if (it->second.is_i() && !it->second.as_i())
                it = j.erase(it);
            else
                ++it;
        tables.push_back(j);

        store_size += j["store_size"].to_i();
        source_ranges_size += j["source_ranges_size"].to_i();
        join_ranges_size += t.join_ranges_.size();
        sink_ranges_size += j["sink_ranges_size"].to_i();
    }

    Json answer;
    answer.set("store_size", store_size)
	.set("source_ranges_size", source_ranges_size)
	.set("join_ranges_size", join_ranges_size)
	.set("valid_ranges_size", sink_ranges_size)
        .set("server_user_time", to_real(ru.ru_utime))
        .set("server_system_time", to_real(ru.ru_stime))
        .set("server_prevalidate_time", prevalidate_time_)
        .set("server_validate_time", validate_time_)
        .set("server_insert_time", insert_time_)
        .set("server_other_time", to_real(ru.ru_utime + ru.ru_stime) - validate_time_ - insert_time_)
        .set("server_max_rss_mb", ru.ru_maxrss / 1024);
    if (SourceRange::allocated_key_bytes)
        answer.set("source_allocated_key_bytes", SourceRange::allocated_key_bytes);
    if (ServerRangeBase::allocated_key_bytes)
        answer.set("sink_allocated_key_bytes", ServerRangeBase::allocated_key_bytes);
    if (SinkRange::invalidate_hit_keys)
        answer.set("invalidate_hits", SinkRange::invalidate_hit_keys);
    if (SinkRange::invalidate_miss_keys)
        answer.set("invalidate_misses", SinkRange::invalidate_miss_keys);
    return answer.set("tables", tables);
}

void Table::print_sources(std::ostream& stream) const {
    stream << source_ranges_;
}

void Server::print(std::ostream& stream) {
    stream << "sources:" << std::endl;
    bool any = false;
    for (auto it = supertable_.lbegin(); it != supertable_.lend(); ++it) {
        assert(it->is_table());
        Table& t = it->table();
        if (!t.source_ranges_.empty()) {
            stream << t.source_ranges_;
            any = true;
        }
    }
    if (!any)
        stream << "<empty>\n";
}

} // namespace
