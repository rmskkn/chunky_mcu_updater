#include "format2.h"
#include "chunky_format.h"
#include "sha256.h"
#include <string.h>

const char *format2_err_str(format2_err_t e) {
    switch (e) {
    case FORMAT2_OK:                  return "ok";
    case FORMAT2_ESHORT:              return "buffer shorter than a fixed field needs";
    case FORMAT2_EMAGIC:              return "bad magic";
    case FORMAT2_EFORMAT:             return "bad format byte";
    case FORMAT2_ESCHEME:             return "scheme is neither Ed25519 nor unsigned";
    case FORMAT2_ESCHEME_UNSUPPORTED: return "Ed25519 scheme: known, but not checkable (no Ed25519 in this tree)";
    case FORMAT2_ERESERVED:           return "reserved bytes not zero";
    case FORMAT2_EGEOMETRY:           return "geometry id mismatch";
    case FORMAT2_ELENGTH:             return "length field out of bounds, or trailing bytes";
    case FORMAT2_ESIGNATURE:          return "seal check failed";
    case FORMAT2_EPRODUCT:            return "wrong product id";
    case FORMAT2_EROLLBACK:           return "security counter below floor";
    case FORMAT2_ESTEP:               return "a step or entry did not parse";
    case FORMAT2_EORDER:              return "step order did not strictly increase";
    case FORMAT2_ERANGE:              return "a block/chunk index is out of range";
    case FORMAT2_EDATALEN:            return "a WRITE's byte count is not one chunk";
    case FORMAT2_EDATA:               return "a WRITE's data does not have the hash it names";
    case FORMAT2_ESOURCE:             return "a COPY's source does not have the hash it names";
    case FORMAT2_ESOURCE_UNREADABLE:  return "a COPY's source is unreadable (ECC fault)";
    case FORMAT2_ENOACTIVE:           return "a COPY step needs ACTIVE and no read_active callback was given";
    default:                          return "unknown format2 error";
    }
}

/* SPEC.md section 2: the first 8 bytes of SHA-256, no domain separation.
 * Matches chunky::hash::ChunkHash::of (github.com/romixlab/chunky_mcu_updater,
 * main, chunky/src/hash.rs) -- confirmed against the vendored golden vectors:
 * every WRITE's carried hash equals this computed over its data. */
void format2_chunk_hash(const uint8_t *data, size_t len, uint8_t out[FORMAT2_CHUNK_HASH_LEN]) {
    uint8_t full[32];
    sha256(data, len, full);
    memcpy(out, full, FORMAT2_CHUNK_HASH_LEN);
}

static int all_zero(const uint8_t *p, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (p[i] != 0u) return 0;
    }
    return 1;
}

/* SPEC.md 4.2: message = "chunky image descriptor v2" || body[0..64]; the
 * domain is exactly these 26 ASCII bytes, no terminator, no length. */
#define SEAL_DOMAIN "chunky image descriptor v2"
#define SEAL_DOMAIN_LEN 26u

/* SPEC.md 4.3: the three stages against a 128-byte SignedDescriptor's
 * 64-byte body and 64-byte seal, its view already made by the caller
 * (either inside a plan header, SPEC.md 5.1, or at the end of a flashed
 * image, SPEC.md section 4). image_len is the device geometry's image_len. */
static format2_err_t check_descriptor(chunky_signed_descriptor_view_t desc,
                                      uint32_t geometry_id, uint32_t product_id,
                                      uint32_t floor, uint32_t image_len,
                                      chunky_descriptor_body_view_t *out_body) {
    const uint8_t *raw = chunky_signed_descriptor_body(desc);
    const uint8_t *seal = chunky_signed_descriptor_seal(desc);
    chunky_descriptor_body_view_t body;
    uint32_t scheme;

    /* stage 1: refusals only, on the raw body bytes as received. All 64 are
     * there, so the view refuses only an unknown scheme; magic and format
     * are still reported before it, read from the raw bytes at their
     * offsets, in SPEC.md 4.3's order. */
    if (chunky_descriptor_body_view(raw, CHUNKY_SIGNED_DESCRIPTOR_BODY_COUNT, &body) != 0) {
        if (memcmp(raw + CHUNKY_DESCRIPTOR_BODY_MAGIC_OFFSET, "CKY2", CHUNKY_DESCRIPTOR_BODY_MAGIC_COUNT) != 0) {
            return FORMAT2_EMAGIC;
        }
        if (raw[CHUNKY_DESCRIPTOR_BODY_FORMAT_OFFSET] != 2u) {
            return FORMAT2_EFORMAT;
        }
        return FORMAT2_ESCHEME;
    }
    if (memcmp(chunky_descriptor_body_magic(body), "CKY2", CHUNKY_DESCRIPTOR_BODY_MAGIC_COUNT) != 0) {
        return FORMAT2_EMAGIC;
    }
    if (chunky_descriptor_body_format(body) != 2u) {
        return FORMAT2_EFORMAT;
    }
    scheme = chunky_scheme_tag(chunky_descriptor_body_scheme(body));
    if (!all_zero(chunky_descriptor_body_reserved(body), CHUNKY_DESCRIPTOR_BODY_RESERVED_COUNT) ||
        !all_zero(chunky_descriptor_body_reserved_end(body), CHUNKY_DESCRIPTOR_BODY_RESERVED_END_COUNT)) {
        return FORMAT2_ERESERVED;
    }
    if (chunky_descriptor_body_image_len(body) != image_len) {
        return FORMAT2_ELENGTH;
    }

    /* stage 2: the seal over the exact 64 body bytes as received. A device
     * is built for exactly one scheme (SPEC.md 4.2); this reader only knows
     * how to check "unsigned". Ed25519 is refused here, before any field
     * below is read, exactly like a real wrong-signature failure would be --
     * the caller must not treat FORMAT2_ESCHEME_UNSUPPORTED as a pass. */
    if (scheme == CHUNKY_SCHEME_ED25519) {
        return FORMAT2_ESCHEME_UNSUPPORTED;
    }
    {
        uint8_t msg[SEAL_DOMAIN_LEN + CHUNKY_DESCRIPTOR_BODY_LEN];
        uint8_t want[32];

        memcpy(msg, SEAL_DOMAIN, SEAL_DOMAIN_LEN);
        memcpy(msg + SEAL_DOMAIN_LEN, raw, CHUNKY_DESCRIPTOR_BODY_LEN);
        sha256(msg, sizeof msg, want);
        if (memcmp(seal, want, 32u) != 0 || !all_zero(seal + 32u, 32u)) {
            return FORMAT2_ESIGNATURE;
        }
    }

    /* stage 3: decode and decide, from the same (now sealed) bytes */
    if (chunky_descriptor_body_geometry_id(body) != geometry_id) {
        return FORMAT2_EGEOMETRY;
    }
    if (chunky_descriptor_body_product_id(body) != product_id) {
        return FORMAT2_EPRODUCT;
    }
    if (chunky_descriptor_body_security_counter(body) < floor) {
        return FORMAT2_EROLLBACK;
    }

    if (out_body != NULL) {
        *out_body = body;
    }
    return FORMAT2_OK;
}

format2_err_t format2_plan_begin(const uint8_t *buf, size_t len,
                                 uint32_t geometry_id, uint32_t product_id,
                                 uint32_t floor, uint32_t image_len,
                                 chunky_descriptor_body_view_t *descriptor_body) {
    chunky_plan_header_view_t h;

    /* SPEC.md 6.1.1: refusals on the header, which is outside the seal. The
     * view refuses only a buffer shorter than the header. */
    if (chunky_plan_header_view(buf, len, &h) != 0) {
        return FORMAT2_ESHORT;
    }
    if (memcmp(chunky_plan_header_magic(h), "CKP2", CHUNKY_PLAN_HEADER_MAGIC_COUNT) != 0) {
        return FORMAT2_EMAGIC;
    }
    if (chunky_plan_header_format(h) != 2u) {
        return FORMAT2_EFORMAT;
    }
    if (chunky_plan_header_geometry_id(h) != geometry_id) {
        return FORMAT2_EGEOMETRY;
    }
    if ((size_t)chunky_plan_header_body_len(h) != len - h.len) {
        return FORMAT2_ELENGTH;
    }

    return check_descriptor(chunky_plan_header_descriptor(h), geometry_id, product_id, floor, image_len,
                            descriptor_body);
}

int format2_plan_walk(const uint8_t *buf, size_t len, uint32_t step_count,
                      uint32_t blocks, uint32_t chunks, uint16_t chunk_len,
                      format2_read_active_fn read_active, void *active_ctx,
                      uint8_t *scratch,
                      format2_step_fn fn, void *ctx) {
    size_t at = CHUNKY_PLAN_HEADER_LEN;
    /* SPEC.md 5.4: every step's position must be strictly greater than the
     * one before it; -1 sorts before any real position (>= 0). */
    long long last_pos = -1;
    /* SPEC.md 5.4: chunks = blocks * chunks_per_block exactly (chunks =
     * slot_len/chunk, blocks = slot_len/erase, chunks_per_block = erase/
     * chunk), so this is chunks_per_block without a separate parameter. */
    uint32_t chunks_per_block;
    uint32_t n;

    if (len < CHUNKY_PLAN_HEADER_LEN) {
        return -(int)FORMAT2_ESHORT;
    }
    if (blocks == 0u) {
        return -(int)FORMAT2_ERANGE; /* no valid geometry has zero erase blocks */
    }
    chunks_per_block = chunks / blocks;

    for (n = 0; n < step_count; n++) {
        chunky_step_view_t v;
        const chunky_step_erase_view_t *erase;
        const chunky_step_copy_view_t *copy;
        const chunky_step_write_view_t *write;
        format2_step_t s;
        long long pos;
        int rc;

        if (at == len) {
            return -(int)FORMAT2_ESTEP;
        }
        /* checks that the step, a WRITE's data included, ends inside buf */
        if (chunky_step_view(buf + at, len - at, &v) != 0) {
            return -(int)FORMAT2_ESTEP;
        }

        memset(&s, 0, sizeof s);
        s.op = chunky_step_tag(&v);
        if (chunky_step_as_erase(&v, &erase) == 0) {
            s.a = chunky_step_erase_block(erase);
            /* SPEC.md 6.2.5: block < blocks */
            if (s.a >= blocks) {
                return -(int)FORMAT2_ERANGE;
            }
            pos = 2LL * (long long)s.a * (long long)chunks_per_block;
        } else if (chunky_step_as_copy(&v, &copy) == 0) {
            s.a = chunky_step_copy_dst(copy);
            s.b = chunky_step_copy_src(copy);
            s.hash = chunky_step_copy_hash(copy);
            /* SPEC.md 6.2.5: dst < chunks, src < chunks */
            if (s.a >= chunks || s.b >= chunks) {
                return -(int)FORMAT2_ERANGE;
            }
            pos = 2LL * (long long)s.a + 1;
        } else if (chunky_step_as_write(&v, &write) == 0) {
            s.a = chunky_step_write_dst(write);
            s.b = chunky_step_write_data_count(write);
            s.hash = chunky_step_write_hash(write);
            s.data = chunky_step_write_data(write);
            /* SPEC.md 6.2.4: a WRITE's count MUST equal chunk */
            if (s.b != chunk_len) {
                return -(int)FORMAT2_EDATALEN;
            }
            /* SPEC.md 6.2.5: dst < chunks */
            if (s.a >= chunks) {
                return -(int)FORMAT2_ERANGE;
            }
            pos = 2LL * (long long)s.a + 1;
        } else {
            return -(int)FORMAT2_ESTEP;
        }
        /* SPEC.md 5.4/6.2.6 */
        if (pos <= last_pos) {
            return -(int)FORMAT2_EORDER;
        }
        last_pos = pos;

        /* SPEC.md 6.2.7, by op: a WRITE's data and a COPY's source MUST
         * have the hash the step names. Destination-blank and read-back
         * checks are the flash port's job (see lib/format2.h's scope
         * note), not this decoder's. */
        if (s.op == CHUNKY_STEP_WRITE) {
            uint8_t got[FORMAT2_CHUNK_HASH_LEN];
            format2_chunk_hash(s.data, s.b, got);
            if (memcmp(got, s.hash, FORMAT2_CHUNK_HASH_LEN) != 0) {
                return -(int)FORMAT2_EDATA;
            }
        } else if (s.op == CHUNKY_STEP_COPY) {
            uint8_t got[FORMAT2_CHUNK_HASH_LEN];
            if (read_active == NULL) {
                return -(int)FORMAT2_ENOACTIVE;
            }
            if (read_active(active_ctx, s.b, scratch, chunk_len) != 0) {
                return -(int)FORMAT2_ESOURCE_UNREADABLE;
            }
            format2_chunk_hash(scratch, chunk_len, got);
            if (memcmp(got, s.hash, FORMAT2_CHUNK_HASH_LEN) != 0) {
                return -(int)FORMAT2_ESOURCE;
            }
        }

        rc = fn(ctx, &s, n);
        if (rc != 0) {
            return rc;
        }
        at += v.len;
    }
    if (at != len) {
        return -(int)FORMAT2_ELENGTH; /* SPEC.md 5.3: never read past body length as steps */
    }
    return 0;
}

format2_err_t format2_manifest_begin(const uint8_t *buf, size_t len,
                                     uint32_t geometry_id, uint16_t *chunks_out) {
    chunky_manifest_header_view_t h;

    if (len < CHUNKY_MANIFEST_HEADER_LEN) {
        return FORMAT2_ESHORT;
    }
    /* All 13 bytes are there, so the view refuses only an unknown scheme
     * (SPEC.md section 3); magic and format are still reported before it,
     * read from the raw bytes at their offsets. */
    if (chunky_manifest_header_view(buf, len, &h) != 0) {
        if (memcmp(buf + CHUNKY_MANIFEST_HEADER_MAGIC_OFFSET, "CKM2", CHUNKY_MANIFEST_HEADER_MAGIC_COUNT) != 0) {
            return FORMAT2_EMAGIC;
        }
        if (buf[CHUNKY_MANIFEST_HEADER_FORMAT_OFFSET] != 2u) {
            return FORMAT2_EFORMAT;
        }
        return FORMAT2_ESCHEME;
    }
    if (memcmp(chunky_manifest_header_magic(h), "CKM2", CHUNKY_MANIFEST_HEADER_MAGIC_COUNT) != 0) {
        return FORMAT2_EMAGIC;
    }
    if (chunky_manifest_header_format(h) != 2u) {
        return FORMAT2_EFORMAT;
    }
    if (chunky_manifest_header_geometry_id(h) != geometry_id) {
        return FORMAT2_EGEOMETRY;
    }
    if (chunks_out != NULL) {
        *chunks_out = chunky_manifest_header_chunks(h);
    }
    return FORMAT2_OK;
}

int format2_manifest_walk(const uint8_t *buf, size_t len, uint32_t entry_count,
                          format2_entry_fn fn, void *ctx) {
    size_t at = CHUNKY_MANIFEST_HEADER_LEN;
    uint32_t n;

    if (len < CHUNKY_MANIFEST_HEADER_LEN) {
        return -(int)FORMAT2_ESHORT;
    }

    for (n = 0; n < entry_count; n++) {
        chunky_chunk_entry_view_t v;
        const chunky_chunk_entry_hash_view_t *hash;
        format2_entry_t e;
        int rc;

        if (at == len) {
            return -(int)FORMAT2_ESTEP;
        }
        if (chunky_chunk_entry_view(buf + at, len - at, &v) != 0) {
            return -(int)FORMAT2_ESTEP;
        }

        e.tag = chunky_chunk_entry_tag(&v);
        e.hash = chunky_chunk_entry_as_hash(&v, &hash) == 0 ? chunky_chunk_entry_hash_hash(hash) : NULL;

        rc = fn(ctx, &e, n);
        if (rc != 0) {
            return rc;
        }
        at += v.len;
    }
    if (at != len) {
        return -(int)FORMAT2_ELENGTH;
    }
    return 0;
}
