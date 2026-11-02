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
    default:                          return "unknown format2 error";
    }
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
 * 64-byte body and 64-byte seal, both already located by the caller
 * (either inside a plan header, SPEC.md 5.1, or at the end of a flashed
 * image, SPEC.md section 4). image_len is the device geometry's image_len. */
static format2_err_t check_descriptor(const uint8_t *body, const uint8_t *seal,
                                      uint32_t geometry_id, uint32_t product_id,
                                      uint32_t floor, uint32_t image_len,
                                      const uint8_t **out_body) {
    uint32_t scheme;

    /* stage 1: refusals only, on the raw body bytes as received */
    if (memcmp(chunky_descriptor_body_magic(body), "CKY2", CHUNKY_DESCRIPTOR_BODY_MAGIC_COUNT) != 0) {
        return FORMAT2_EMAGIC;
    }
    if (chunky_descriptor_body_format(body) != 2u) {
        return FORMAT2_EFORMAT;
    }
    scheme = chunky_scheme_tag(chunky_descriptor_body_scheme(body));
    if (scheme != CHUNKY_SCHEME_ED25519 && scheme != CHUNKY_SCHEME_UNSIGNED) {
        return FORMAT2_ESCHEME;
    }
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
        memcpy(msg + SEAL_DOMAIN_LEN, body, CHUNKY_DESCRIPTOR_BODY_LEN);
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
                                 const uint8_t **descriptor_body) {
    const uint8_t *desc;
    const uint8_t *body;
    const uint8_t *seal;

    /* SPEC.md 6.1.1: refusals on the header, which is outside the seal */
    if (len < CHUNKY_PLAN_HEADER_LEN) {
        return FORMAT2_ESHORT;
    }
    if (memcmp(chunky_plan_header_magic(buf), "CKP2", CHUNKY_PLAN_HEADER_MAGIC_COUNT) != 0) {
        return FORMAT2_EMAGIC;
    }
    if (chunky_plan_header_format(buf) != 2u) {
        return FORMAT2_EFORMAT;
    }
    if (chunky_plan_header_geometry_id(buf) != geometry_id) {
        return FORMAT2_EGEOMETRY;
    }
    if ((size_t)chunky_plan_header_body_len(buf) != len - CHUNKY_PLAN_HEADER_LEN) {
        return FORMAT2_ELENGTH;
    }

    desc = chunky_plan_header_descriptor(buf);
    body = chunky_signed_descriptor_body(desc);
    seal = chunky_signed_descriptor_seal(desc);
    return check_descriptor(body, seal, geometry_id, product_id, floor, image_len, descriptor_body);
}

int format2_plan_walk(const uint8_t *buf, size_t len, uint32_t step_count,
                      uint32_t chunks_per_block,
                      format2_step_fn fn, void *ctx) {
    const uint8_t *at = buf + CHUNKY_PLAN_HEADER_LEN;
    const uint8_t *end = buf + len;
    /* SPEC.md 5.4: every step's position must be strictly greater than the
     * one before it; -1 sorts before any real position (>= 0). */
    long long last_pos = -1;
    uint32_t n;

    if (len < CHUNKY_PLAN_HEADER_LEN) {
        return -(int)FORMAT2_ESHORT;
    }

    for (n = 0; n < step_count; n++) {
        const uint8_t *t;
        chunky_step_tail_t tl;
        format2_step_t s;
        long long pos;
        int rc;

        if (at >= end) {
            return -(int)FORMAT2_ESTEP;
        }
        t = at + CHUNKY_STEP_TAG_LEN;
        memset(&tl, 0, sizeof tl); /* chunky_step_tail only sets write_data for a WRITE */
        if (chunky_step_tail(at, &t, end, &tl) != 0) {
            return -(int)FORMAT2_ESTEP;
        }

        memset(&s, 0, sizeof s);
        s.op = chunky_step_tag(at);
        switch (s.op) {
        case CHUNKY_STEP_ERASE:
            s.a = chunky_step_erase_block(tl.head);
            pos = 2LL * (long long)s.a * (long long)chunks_per_block;
            break;
        case CHUNKY_STEP_COPY:
            s.a = chunky_step_copy_dst(tl.head);
            s.b = chunky_step_copy_src(tl.head);
            s.hash = chunky_step_copy_hash(tl.head);
            pos = 2LL * (long long)s.a + 1;
            break;
        case CHUNKY_STEP_WRITE:
            s.a = chunky_step_write_dst(tl.head);
            s.b = chunky_step_write_data_count(tl.head);
            s.hash = chunky_step_write_hash(tl.head);
            s.data = tl.write_data;
            pos = 2LL * (long long)s.a + 1;
            break;
        default:
            return -(int)FORMAT2_ESTEP;
        }
        if (pos <= last_pos) {
            return -(int)FORMAT2_EORDER;
        }
        last_pos = pos;

        rc = fn(ctx, &s, n);
        if (rc != 0) {
            return rc;
        }
        at = t;
    }
    if (at != end) {
        return -(int)FORMAT2_ELENGTH; /* SPEC.md 5.3: never read past body length as steps */
    }
    return 0;
}

format2_err_t format2_manifest_begin(const uint8_t *buf, size_t len,
                                     uint32_t geometry_id, uint16_t *chunks_out) {
    if (len < CHUNKY_MANIFEST_HEADER_LEN) {
        return FORMAT2_ESHORT;
    }
    if (memcmp(chunky_manifest_header_magic(buf), "CKM2", CHUNKY_MANIFEST_HEADER_MAGIC_COUNT) != 0) {
        return FORMAT2_EMAGIC;
    }
    if (chunky_manifest_header_format(buf) != 2u) {
        return FORMAT2_EFORMAT;
    }
    if (chunky_manifest_header_geometry_id(buf) != geometry_id) {
        return FORMAT2_EGEOMETRY;
    }
    if (chunks_out != NULL) {
        *chunks_out = chunky_manifest_header_chunks(buf);
    }
    return FORMAT2_OK;
}

int format2_manifest_walk(const uint8_t *buf, size_t len, uint32_t entry_count,
                          format2_entry_fn fn, void *ctx) {
    const uint8_t *at = buf + CHUNKY_MANIFEST_HEADER_LEN;
    const uint8_t *end = buf + len;
    uint32_t n;

    if (len < CHUNKY_MANIFEST_HEADER_LEN) {
        return -(int)FORMAT2_ESHORT;
    }

    for (n = 0; n < entry_count; n++) {
        const uint8_t *t;
        chunky_chunk_entry_tail_t tl;
        format2_entry_t e;
        int rc;

        if (at >= end) {
            return -(int)FORMAT2_ESTEP;
        }
        t = at + CHUNKY_CHUNK_ENTRY_TAG_LEN;
        if (chunky_chunk_entry_tail(at, &t, end, &tl) != 0) {
            return -(int)FORMAT2_ESTEP;
        }

        e.tag = chunky_chunk_entry_tag(at);
        e.hash = (e.tag == CHUNKY_CHUNK_ENTRY_HASH) ? chunky_chunk_entry_hash_hash(tl.head) : NULL;

        rc = fn(ctx, &e, n);
        if (rc != 0) {
            return rc;
        }
        at = t;
    }
    if (at != end) {
        return -(int)FORMAT2_ELENGTH;
    }
    return 0;
}
