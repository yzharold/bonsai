#ifndef _TX_H__
#define _TX_H__
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include "util.h"
#include "klib/kthread.h"
#include "feature_min.h"
#include "counter.h"
#include "linear/linear.h"
#include "diskarray.h"


template<typename T>
size_t get_n_occ(T *hash) {
    size_t ret(0);
    for(khiter_t ki(0); ki != kh_end(hash); ++ki) ret += !!kh_exist(hash, ki);
    return ret;
}


namespace emp {


using namespace std::literals;

/*
1. Get taxids you need.
2. Create the map over and the map back.
3. Move the new id/file path mapping into workflow for logging.
 */
class TaxonomyReformation {
protected:
    khash_t(p)           *pmap_;
    khash_t(name)    *name_map_;
    std::vector<tax_t> old_ids_;
    std::unordered_map<tax_t, std::vector<std::string>> path_map;
    std::unordered_map<tax_t, std::string> newid_path_map;
    khash_t(p) *old_to_new_;
    uint64_t        counter_:62;
    uint64_t          filled_:1;
    uint64_t  panic_on_undef_:1;
public:
    template<typename StrCon>
    TaxonomyReformation(const char *name_path, const StrCon &paths,
                        const khash_t(p) *old_tax, bool panic_on_undef=false):
        pmap_{kh_init(p)},
        name_map_{build_name_hash(name_path)},
        old_ids_{0, 1},
        old_to_new_(kh_init(p)),
        counter_(1), filled_(false), panic_on_undef_(panic_on_undef)
    {
        LOG_DEBUG("Initialized default stuff. name map size: %zu\n", kh_size(name_map_));
        // Copy input hash map
        khash_t(p) *ct(kh_init(p));
        kh_resize(p, ct, kh_size(old_tax));
        assert(ct->n_buckets == old_tax->n_buckets && (std::to_string(ct->n_buckets) + "," + std::to_string(old_tax->n_buckets)).data());
        std::memcpy(ct->keys, old_tax->keys, sizeof(tax_t) * old_tax->n_buckets);
        std::memcpy(ct->vals, old_tax->vals, sizeof(tax_t) * old_tax->n_buckets);
        std::memcpy(ct->flags, old_tax->flags, __ac_fsize(old_tax->n_buckets));
#define CP(x) ct->x = old_tax->x
        CP(n_buckets); CP(size); CP(n_occupied); CP(upper_bound);
#undef CP
        LOG_DEBUG("Copied stuff. copied map size: %zu\n", kh_size(ct));
#if !NDEBUG
        for(khint_t ki(0); ki < kh_end(ct); ++ki)
            if(kh_exist(ct, ki))
                assert(kh_get(p, old_tax, kh_key(ct, ki)) != kh_end(old_tax));
#endif
        LOG_DEBUG("Map was correctly copied.\n");

        // Add the root of the tree to our pmap.
        int khr;
        khint_t ki(kh_put(p, pmap_, tax_t(counter_), &khr));
        kh_val(pmap_, ki) = 0;
        fill_path_map(paths);
        // Deterministic seeding that still varies run to run.
        std::mt19937 mt(std::hash<size_t>()(kh_size(old_tax) * path_map.size()));
        for(auto &pair: path_map) {
            if(pair.second.size() > 1) {
                for(auto &path: pair.second) {
                    tax_t tmp(mt());
                    while(kh_get(p, ct, tmp) != kh_end(ct)) tmp = mt();
                    path_map[tmp] = {path};
                    ki = kh_put(p, ct, tmp, &khr);
                    kh_val(ct, ki) = pair.first;
                    newid_path_map[tmp] = path;
                }
                std::vector<std::string> vec;
                std::swap(vec, pair.second);
            }
        }
        std::vector<tax_t> insertion_order;
        insertion_order.reserve(path_map.size());
        for(const auto &pair: path_map) insertion_order.push_back(pair.first);
        assert(insertion_order.size() == std::unordered_set<tax_t>(insertion_order.begin(), insertion_order.end()).size() &&
               "Some tax ids are being repeated in this array.");
        pdqsort(std::begin(insertion_order), std::end(insertion_order),
                [old_tax] (const tax_t a, const tax_t b) {
            return node_depth(old_tax, a) < node_depth(old_tax, b);
        });
        for(const auto tax: insertion_order) {
            int khr;
            khiter_t ki(kh_put(p, old_to_new_, tax, &khr));
            kh_val(old_to_new_, tax) = old_ids_.size();
            old_ids_.emplace_back(tax);
            ki = kh_put(p, pmap_, static_cast<tax_t>(old_ids_.size()), &khr);
            kh_val(pmap_, ki) = kh_val(old_to_new_, kh_get(p, old_to_new_, kh_val(ct, kh_get(p, ct, tax))));
            // TODO: remove \.at check when we're certain it's working.
        }
        khash_destroy(ct);
        // Now convert name_map from old to new.
        for(khint_t ki(0); ki < kh_size(name_map_); ++ki)
            if(kh_exist(name_map_, ki))
                kh_val(name_map_, ki) = kh_val(old_to_new_, kh_get(p, old_to_new_, kh_val(name_map_, ki)));

        LOG_DEBUG("Paths to genomes with new subtax elements:\n\n\n%s", newtaxprintf().data());
        STLFREE(path_map);
    }

    ks::string newtaxprintf() {
        ks::string ret("#New ID\tGenome path (NEW FIRST)\n");
        for(const auto &pair: newid_path_map)
            ret.sprintf("%u\t%s\n", pair.first, pair.second.data());
        ret += "#(Original taxonomy)\n";
        for(const auto &pair: path_map)
            ret.sprintf("%u\t%s\n", pair.first, pair.second.data());
        return ret;
    }
    void fnewtaxprintf(std::FILE *fp) {
        auto taxstr(newtaxprintf());
        taxstr.write(fp);
    }

    template<typename T>
    void fill_path_map(const T &container) {
        for(const auto &path: container) {
            const tax_t id(get_taxid(get_cstr(path), name_map_));
            if(id == tax_t(-1)) {
                if(panic_on_undef_)
                    throw std::runtime_error(ks::sprintf("Tax id not found in path %s. Skipping. This can be fixed by augmenting the name dictionary file.\n", get_cstr(path)).data());
                else
                    LOG_WARNING("Tax id not found in path %s. Skipping. This can be fixed by augmenting the name dictionary file.\n", get_cstr(path));
                continue;
            }
            path_map[id].push_back(path);
        }
    }

    // Convenience functions
    tax_t parent(tax_t child) const {
        const auto ind(kh_get(p, pmap_, child));
        return ind == kh_end(pmap_) ? tax_t(-1): kh_val(pmap_, ind);
    }
    void write_name_map(const char *fn) {
        gzFile fp(gzopen(fn, "wb"));
        if(fp == nullptr) throw std::runtime_error(ks::sprintf("Could not open file at %s for writing", fn).data());
        gzbuffer(fp, 1 << 18);
        for(const auto &pair: newid_path_map) {
            gzprintf(fp, "%s\t%u\n", pair.second.data(), pair.first);
        }
        gzclose(fp);
    }
    void write_old_to_new(const char *fn) {
        gzFile fp(gzopen(fn, "wb"));
        if(fp == nullptr) throw std::runtime_error(ks::sprintf("Could not open file at %s for writing", fn).data());
        gzbuffer(fp, 1 << 18);
        gzprintf(fp, "#Old\tNew\n");
        for(khiter_t ki(0); ki < kh_end(old_to_new_); ++ki)
            if(kh_exist(old_to_new_, ki))
                gzprintf(fp, "%u\t%u\n", kh_key(old_to_new_, ki), kh_val(old_to_new_, ki));
        gzclose(fp);
    }
    void write_new_to_old(const char *fn) {
        gzFile fp(gzopen(fn, "wb"));
        if(fp == nullptr) throw std::runtime_error(ks::sprintf("Could not open file at %s for writing", fn).data());
        gzbuffer(fp, 1 << 18);
        for(const auto &el: old_ids_) gzwrite(fp, (void *)&el, sizeof(el));
        gzclose(fp);
    }
    void clear() {
        if(name_map_) khash_destroy(name_map_);
        if(pmap_) khash_destroy(pmap_);
        if(old_to_new_) khash_destroy(old_to_new_);
        STLFREE(old_ids_);
        STLFREE(path_map);
        STLFREE(newid_path_map);
    }
    ~TaxonomyReformation() {clear();}
};
using concise_tax_t = TaxonomyReformation;
std::vector<tax_t> build_new2old_map(const char *str, size_t bufsz=1<<16);
INLINE std::vector<tax_t> build_new2old_map(const std::string &str, size_t bufsz) { return build_new2old_map(str.data(), bufsz);}
std::vector<tax_t> binary_new2old_map(const char *);
void bitmap_filler_helper(void *data_, long index, int tid);
struct bf_helper_t {
    const Spacer &sp_;
    const std::vector<std::string> &paths_;
    const std::vector<tax_t> &taxes_;
    const khash_t(64) *h_;
    ba::MMapTaxonomyBitmap &bm_;
    const bool canonicalize_;
};



} // namespace emp

#endif // #ifndef _TX_H__
