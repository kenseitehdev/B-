#include "internal.h"

#define STB_IMAGE_IMPLEMENTATION
#include "./stb/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "./stb/stb_image_write.h"

static xf_Str *img_key(const char *s) {
    return xf_str_from_cstr(s);
}

static xf_Value img_map_get_cstr(xf_map_t *m, const char *key) {
    xf_Str *k = img_key(key);
    xf_Value v = xf_map_get(m, k);
    xf_str_release(k);
    return v;
}

static bool img_map_set_num(xf_map_t *m, const char *key, double n) {
    xf_Str *k = img_key(key);
    if (!k) return false;
    xf_map_set(m, k, xf_val_ok_num(n));
    xf_str_release(k);
    return true;
}

static bool img_map_set_str(xf_map_t *m, const char *key, const char *s) {
    xf_Str *k = img_key(key);
    xf_Str *sv = xf_str_from_cstr(s ? s : "");
    if (!k || !sv) {
        xf_str_release(k);
        xf_str_release(sv);
        return false;
    }
    xf_Value tmp = xf_val_ok_str(sv);
xf_map_set(m, k, tmp);
xf_value_release(tmp);
    xf_str_release(k);
    xf_str_release(sv);
    return true;
}

static bool img_map_set_boolnum(xf_map_t *m, const char *key, bool b) {
    return img_map_set_num(m, key, b ? 1.0 : 0.0);
}

static bool img_map_set_arr(xf_map_t *m, const char *key, xf_arr_t *a) {
    xf_Str *k = img_key(key);
    if (!k || !a) {
        xf_str_release(k);
        return false;
    }
    xf_map_set(m, k, xf_val_ok_arr(a));
    xf_str_release(k);
    return true;
}

static bool img_value_to_size(xf_Value v, size_t *out) {
    if (!out) return false;
    if (v.state != XF_STATE_OK) return false;

    xf_Value n = xf_coerce_num(v);
    if (n.state != XF_STATE_OK) {
        xf_value_release(n);
        return false;
    }

    double d = n.data.num;
    xf_value_release(n);

    if (d < 0.0) return false;
    *out = (size_t)d;
    return true;
}

static unsigned char img_clamp_byte_from_value(xf_Value v, bool normalized, bool *ok) {
    if (ok) *ok = false;
    if (v.state != XF_STATE_OK) return 0;

    xf_Value n = xf_coerce_num(v);
    if (n.state != XF_STATE_OK) {
        xf_value_release(n);
        return 0;
    }

    double d = n.data.num;
    xf_value_release(n);

    if (normalized) d *= 255.0;
    if (d < 0.0) d = 0.0;
    if (d > 255.0) d = 255.0;

    if (ok) *ok = true;
    return (unsigned char)(d + 0.5);
}

static xf_tuple_t *img_make_rgb_tuple(unsigned char r, unsigned char g, unsigned char b, bool normalized) {
    double scale = normalized ? (1.0 / 255.0) : 1.0;
    xf_Value items[3];
    items[0] = xf_val_ok_num((double)r * scale);
    items[1] = xf_val_ok_num((double)g * scale);
    items[2] = xf_val_ok_num((double)b * scale);
    return xf_tuple_new(items, 3); /* steals ownership */
}

static xf_Value ci_vectorize(xf_Value *args, size_t argc) {
    NEED(1);

    const char *path = NULL;
    size_t plen = 0;
    if (!arg_str(args, argc, 0, &path, &plen)) return propagate(args, argc);

    const char *mode = "rgb";
    size_t modelen = 3;
    if (argc >= 2) {
        if (!arg_str(args, argc, 1, &mode, &modelen)) return propagate(args, argc);
    }

    bool normalized = false;
    if (argc >= 3) {
        double dn = 0.0;
        if (!arg_num(args, argc, 2, &dn)) return propagate(args, argc);
        normalized = (dn != 0.0);
    }

    int req_channels = 3;
    if (strcmp(mode, "rgb") == 0) {
        req_channels = 3;
    } else if (strcmp(mode, "gray") == 0 || strcmp(mode, "grey") == 0) {
        req_channels = 1;
    } else if (strcmp(mode, "rgba") == 0) {
        req_channels = 4;
    } else {
        return xf_val_nav(XF_TYPE_MAP);
    }

    int w = 0, h = 0, src_channels = 0;
    unsigned char *pixels = stbi_load(path, &w, &h, &src_channels, req_channels);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return xf_val_nav(XF_TYPE_MAP);
    }

    /*
     * Canonical v1 contract:
     * - always expose RGB tuples
     * - data is arr<tuple<num,num,num>>
     * - channels reports 3
     *
     * gray gets expanded to (v,v,v)
     * rgba drops alpha
     */
    xf_arr_t *data = xf_arr_new();
    if (!data) {
        stbi_image_free(pixels);
        return xf_val_nav(XF_TYPE_MAP);
    }

    size_t pixel_count = (size_t)w * (size_t)h;
    for (size_t i = 0; i < pixel_count; i++) {
        unsigned char r = 0, g = 0, b = 0;

        if (req_channels == 1) {
            unsigned char v = pixels[i];
            r = v; g = v; b = v;
        } else if (req_channels == 4) {
            size_t base = i * 4;
            r = pixels[base + 0];
            g = pixels[base + 1];
            b = pixels[base + 2];
        } else {
            size_t base = i * 3;
            r = pixels[base + 0];
            g = pixels[base + 1];
            b = pixels[base + 2];
        }

        xf_tuple_t *t = img_make_rgb_tuple(r, g, b, normalized);
        if (!t) {
            stbi_image_free(pixels);
            xf_arr_release(data);
            return xf_val_nav(XF_TYPE_MAP);
        }

        xf_arr_push(data, xf_val_ok_tuple(t));
        xf_tuple_release(t);
    }

    stbi_image_free(pixels);

    xf_map_t *out = xf_map_new();
    if (!out) {
        xf_arr_release(data);
        return xf_val_nav(XF_TYPE_MAP);
    }

    if (!img_map_set_num(out, "width", (double)w) ||
        !img_map_set_num(out, "height", (double)h) ||
        !img_map_set_num(out, "channels", 3.0) ||
        !img_map_set_str(out, "mode", "rgb") ||
        !img_map_set_boolnum(out, "normalized", normalized) ||
        !img_map_set_arr(out, "data", data)) {
        xf_arr_release(data);
        xf_map_release(out);
        return xf_val_nav(XF_TYPE_MAP);
    }

    xf_arr_release(data);

    xf_Value v = xf_val_ok_map(out);
    xf_map_release(out);
    return v;
}


static bool img_ext_eq(const char *path, const char *ext) {
    if (!path || !ext) return false;

    const char *dot = strrchr(path, '.');
    if (!dot) return false;

    while (*dot && *ext) {
        unsigned char a = (unsigned char)*dot++;
        unsigned char b = (unsigned char)*ext++;
        if (tolower(a) != tolower(b)) return false;
    }

    return *dot == '\0' && *ext == '\0';
}

static int img_write_by_path(const char *path, int width, int height, const unsigned char *buf) {
    if (!path || !buf || width <= 0 || height <= 0) return 0;

    if (img_ext_eq(path, ".png")) {
        return stbi_write_png(path, width, height, 3, buf, width * 3);
    }
    if (img_ext_eq(path, ".jpg") || img_ext_eq(path, ".jpeg")) {
        return stbi_write_jpg(path, width, height, 3, buf, 90);
    }
    if (img_ext_eq(path, ".bmp")) {
        return stbi_write_bmp(path, width, height, 3, buf);
    }
    if (img_ext_eq(path, ".tga")) {
        return stbi_write_tga(path, width, height, 3, buf);
    }

    return stbi_write_png(path, width, height, 3, buf, width * 3);
}

static xf_Value ci_unvectorize(xf_Value *args, size_t argc) {
    NEED(2);

    if (args[0].state != XF_STATE_OK || args[0].type != XF_TYPE_MAP || !args[0].data.map)
        return xf_val_nav(XF_TYPE_NUM);

    const char *path = NULL;
    size_t plen = 0;
    if (!arg_str(args, argc, 1, &path, &plen)) return propagate(args, argc);

    xf_map_t *m = args[0].data.map;

    xf_Value vw = img_map_get_cstr(m, "width");
    xf_Value vh = img_map_get_cstr(m, "height");
    xf_Value vc = img_map_get_cstr(m, "channels");
    xf_Value vd = img_map_get_cstr(m, "data");
    xf_Value vn = img_map_get_cstr(m, "normalized");

    size_t width = 0, height = 0, channels = 0;
    bool normalized = false;

    bool ok =
        img_value_to_size(vw, &width) &&
        img_value_to_size(vh, &height) &&
        img_value_to_size(vc, &channels);

    if (vn.state == XF_STATE_OK) {
        xf_Value nn = xf_coerce_num(vn);
        if (nn.state == XF_STATE_OK) normalized = (nn.data.num != 0.0);
        xf_value_release(nn);
    }

    if (!ok ||
        width == 0 ||
        height == 0 ||
        channels != 3 ||
        vd.state != XF_STATE_OK ||
        vd.type != XF_TYPE_ARR ||
        !vd.data.arr) {
        xf_value_release(vw);
        xf_value_release(vh);
        xf_value_release(vc);
        xf_value_release(vd);
        xf_value_release(vn);
        return xf_val_nav(XF_TYPE_NUM);
    }

    xf_arr_t *data = vd.data.arr;
    size_t expected = width * height;
    if (data->len != expected) {
        xf_value_release(vw);
        xf_value_release(vh);
        xf_value_release(vc);
        xf_value_release(vd);
        xf_value_release(vn);
        return xf_val_nav(XF_TYPE_NUM);
    }

    unsigned char *buf = (unsigned char *)malloc(expected * 3);
    if (!buf) {
        xf_value_release(vw);
        xf_value_release(vh);
        xf_value_release(vc);
        xf_value_release(vd);
        xf_value_release(vn);
        return xf_val_nav(XF_TYPE_NUM);
    }

    bool valid = true;

    for (size_t i = 0; i < expected; i++) {
        xf_Value px = data->items[i];
        if (px.state != XF_STATE_OK || px.type != XF_TYPE_TUPLE || !px.data.tuple) {
            valid = false;
            break;
        }

        if (xf_tuple_len(px.data.tuple) != 3) {
            valid = false;
            break;
        }

        xf_Value vr = xf_tuple_get(px.data.tuple, 0);
        xf_Value vg = xf_tuple_get(px.data.tuple, 1);
        xf_Value vb = xf_tuple_get(px.data.tuple, 2);

        bool okr = false, okg = false, okb = false;
        unsigned char r = img_clamp_byte_from_value(vr, normalized, &okr);
        unsigned char g = img_clamp_byte_from_value(vg, normalized, &okg);
        unsigned char b = img_clamp_byte_from_value(vb, normalized, &okb);

        if (!okr || !okg || !okb) {
            valid = false;
            break;
        }

        size_t base = i * 3;
        buf[base + 0] = r;
        buf[base + 1] = g;
        buf[base + 2] = b;
    }

    int wrote = 0;
    if (valid) {
        wrote = img_write_by_path(path, (int)width, (int)height, buf);
    }

    free(buf);
    xf_value_release(vw);
    xf_value_release(vh);
    xf_value_release(vc);
    xf_value_release(vd);
    xf_value_release(vn);

    if (!valid || !wrote) return xf_val_nav(XF_TYPE_NUM);
    return xf_val_ok_num(1.0);
}

xf_module_t *build_img(void) {
    xf_module_t *m = xf_module_new("core.img");
    FN("vectorize",   XF_TYPE_MAP, ci_vectorize);
    FN("unvectorize", XF_TYPE_NUM, ci_unvectorize);
    return m;
}