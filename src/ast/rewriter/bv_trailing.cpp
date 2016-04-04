/*++
 Copyright (c) 2016 Microsoft Corporation

 Module Name:

  bv_trailing.cpp

 Abstract:


 Author:

 Mikolas Janota (MikolasJanota)

 Revision History:
--*/
#include"bv_trailing.h"
#include"bv_decl_plugin.h"
#include"ast_smt2_pp.h"

#define  TRAILING_DEPTH 4

struct bv_trailing::imp {
    typedef rational numeral;
    typedef obj_map<expr, std::pair<unsigned,unsigned> > map;
    mk_extract_proc&     m_mk_extract;
    bv_util&             m_util;
    ast_manager&         m_m;
    map                  m_count_cache[TRAILING_DEPTH + 1];

    imp(mk_extract_proc& mk_extract)
        : m_mk_extract(mk_extract)
        , m_util(mk_extract.bvutil())
        , m_m(mk_extract.m())
        { }

    virtual ~imp() {
        reset_cache();
    }

    ast_manager & m() const { return m_util.get_manager(); }

    void cache(unsigned depth, expr * e, unsigned min, unsigned max) {
        SASSERT(depth <= TRAILING_DEPTH);
        m().inc_ref(e);
        m_count_cache[depth].insert(e, std::make_pair(min, max));
        TRACE("bv-trailing", tout << "caching@" << depth <<": " << mk_ismt2_pp(e, m()) << '[' << m_util.get_bv_size(e) << "]\n: " << min << '-' << max << "\n";);
    }

    bool is_cached(unsigned depth, expr * e, unsigned& min, unsigned& max) {
        SASSERT(depth <= TRAILING_DEPTH);
        const map::obj_map_entry * const oe = m_count_cache[depth].find_core(e);
        if (oe == NULL) return false;
        min = oe->get_data().m_value.first;
        max = oe->get_data().m_value.second;
        TRACE("bv-trailing", tout << "cached@" << depth << ": " << mk_ismt2_pp(e, m()) << '[' << m_util.get_bv_size(e) << "]\n: " << min << '-' << max << "\n";);
        return true;
    }


    void reset_cache() {
        for (unsigned i = 0; i <= TRAILING_DEPTH; ++i) {
            map::iterator it = m_count_cache[i].begin();
            map::iterator end = m_count_cache[i].end();
            for (; it != end; ++it) m().dec_ref(it->m_key);
            m_count_cache[i].reset();
        }
    }

    br_status eq_remove_trailing(expr * e1, expr * e2, expr_ref& result) {
        TRACE("bv-trailing", tout << mk_ismt2_pp(e1, m()) << "\n=\n" << mk_ismt2_pp(e2, m()) << "\n";);
        SASSERT(m_util.is_bv(e1) && m_util.is_bv(e2));
        SASSERT(m_util.get_bv_size(e1) == m_util.get_bv_size(e2));
        unsigned max1, min1, max2, min2;
        count_trailing(e1, min1, max1, TRAILING_DEPTH);
        count_trailing(e2, min2, max2, TRAILING_DEPTH);
        if (min1 > max2 || min2 > max1) {
            result = m().mk_false();
            return BR_DONE;
        }
        const unsigned min = std::min(min1, min2);
        if (min == 0) {
            result = m().mk_eq(e1, e2);
            return BR_FAILED;
        }
        const unsigned sz  = m_util.get_bv_size(e1);
        if (min == sz) { // unlikely but we check anyhow for safety
            result = m().mk_true();
            return BR_DONE;
        }
        expr_ref out1(m());
        expr_ref out2(m());
        remove_trailing(e1, min, out1, TRAILING_DEPTH);
        remove_trailing(e2, min, out2, TRAILING_DEPTH);
        result = m().mk_eq(out1, out2);
        return BR_REWRITE2;
    }

    unsigned remove_trailing_add(app * a, unsigned n, expr_ref& result, unsigned depth) {
        SASSERT(m_util.is_bv_add(a));
        const unsigned num  = a->get_num_args();
        if (depth <= 1) {
            result = a;
            return 0;
        }
        unsigned min, max;
        count_trailing(a, min, max, depth);
        const unsigned to_rm = std::min(min, n);
        if (to_rm == 0) {
            result = a;
            return 0;
        }

        const unsigned sz = m_util.get_bv_size(a);

        if (to_rm == sz) {
            result = NULL;
            return sz;
        }

        expr_ref_vector new_args(m());
        expr_ref tmp(m());
        for (unsigned i = 0; i < num; ++i) {
            expr * const curr = a->get_arg(i);
            const unsigned crm = remove_trailing(curr, to_rm, tmp, depth - 1);
            new_args.push_back(tmp);
            SASSERT(crm == to_rm);
        }
        result = m().mk_app(m_util.get_fid(), OP_BADD, new_args.size(), new_args.c_ptr());
        return to_rm;
    }

    unsigned remove_trailing_mul(app * a, unsigned n, expr_ref& result, unsigned depth) {
        SASSERT(m_util.is_bv_mul(a));
        const unsigned num  = a->get_num_args();
        if (depth <= 1 || !num) {
            result = a;
            return 0;
        }
        expr_ref tmp(m());
        expr * const coefficient = a->get_arg(0);
        const unsigned retv = remove_trailing(coefficient, n, tmp, depth - 1);
        SASSERT(retv <= n);
        if (retv == 0) {
            result = a;
            return 0;
        }
        expr_ref_vector new_args(m());
        numeral c_val;
        unsigned c_sz;
        if (!m_util.is_numeral(tmp, c_val, c_sz) || !c_val.is_one())
            new_args.push_back(tmp);
        const unsigned sz = m_util.get_bv_size(coefficient);
        const unsigned new_sz = sz - retv;

        if (!new_sz) {
            result = NULL;
            return retv;
        }

        SASSERT(m_util.get_bv_size(tmp) == new_sz);
        for (unsigned i = 1; i < num; i++) {
            expr * const curr = a->get_arg(i);
            new_args.push_back(m_mk_extract(new_sz - 1, 0, curr));
        }
        switch (new_args.size()) {
        case 0: result = m_util.mk_numeral(1, new_sz); break;
        case 1: result = new_args.get(0); break;
        default: result = m().mk_app(m_util.get_fid(), OP_BMUL, new_args.size(), new_args.c_ptr());
        }
        return retv;
    }

    unsigned remove_trailing_concat(app * a, unsigned n, expr_ref& result, unsigned depth) {
        SASSERT(m_util.is_concat(a));
        if (depth <= 1) {
            result = a;
            return 0;
        }
        unsigned num  = a->get_num_args();
        unsigned retv = 0;
        unsigned i = num;
        expr_ref new_last(NULL, m());
        while (i && retv < n) {
            i--;
            expr * const curr = a->get_arg(i);
            const unsigned cur_rm = remove_trailing(curr, n, new_last, depth - 1);
            const unsigned curr_sz = m_util.get_bv_size(curr);
            retv += cur_rm;
            if (cur_rm < curr_sz) break;
        }
        if (retv == 0) {
            result = a;
            return 0;
        }

        if (!i) {// all args eaten completely
            SASSERT(new_last.get() == NULL);
            SASSERT(retv == m_util.get_bv_size(a));
            result = NULL;
            return retv;
        }

        expr_ref_vector new_args(m());
        for (size_t j=0; j<i;++j)
            new_args.push_back(a->get_arg(j));
        if (new_last.get()) new_args.push_back(new_last);
        result = new_args.size() == 1 ? new_args.get(0)
                                      : m_util.mk_concat(new_args.size(), new_args.c_ptr());
        return retv;
    }

    unsigned remove_trailing(size_t max_rm, numeral& a) {
        numeral two(2);
        unsigned retv = 0;
        while (max_rm && a.is_even()) {
            div(a, two, a);
            ++retv;
            --max_rm;
        }
        return retv;
    }

    unsigned remove_trailing(expr * e, unsigned n, expr_ref& result, unsigned depth) {
        const unsigned retv = remove_trailing_core(e, n, result, depth);
        CTRACE("bv-trailing", result.get(),  tout << mk_ismt2_pp(e, m()) << "\n--->\n" <<  mk_ismt2_pp(result.get(), m())  << "\n";);
        CTRACE("bv-trailing", !result.get(), tout << mk_ismt2_pp(e, m()) << "\n---> [EMPTY]\n";);
        return retv;
    }

    unsigned remove_trailing_core(expr * e, unsigned n, expr_ref& result, unsigned depth) {
        SASSERT(m_util.is_bv(e));
        if (!depth) return 0;
        if (n == 0) return 0;
        unsigned sz;
        unsigned retv = 0;
        numeral e_val;
        if (m_util.is_numeral(e, e_val, sz)) {
            retv = remove_trailing(n, e_val);
            const unsigned new_sz = sz - retv;
            result = new_sz ? (retv ? m_util.mk_numeral(e_val, new_sz) : e) : NULL;
            return retv;
        }
        if (m_util.is_bv_mul(e))
            return remove_trailing_mul(to_app(e), n, result, depth);
        if (m_util.is_bv_add(e))
            return remove_trailing_add(to_app(e), n, result, depth);
        if (m_util.is_concat(e))
            return remove_trailing_concat(to_app(e), n, result, depth);
        return 0;
    }

    void count_trailing(expr * e, unsigned& min, unsigned& max, unsigned depth) {
        if (is_cached(depth, e, min, max))
            return;
        SASSERT(e && m_util.is_bv(e));
        count_trailing_core(e, min, max, depth);
        TRACE("bv-trailing", tout << mk_ismt2_pp(e, m()) << "\n:" << min << " - " << max << "\n";);
        SASSERT(min <= max);
        SASSERT(max <= m_util.get_bv_size(e));
        cache(depth, e, min, max);  // store result into the cache
    }

    void count_trailing_concat(app * a, unsigned& min, unsigned& max, unsigned depth) {
        if (depth <= 1) {
            min = 0;
            max = m_util.get_bv_size(a);
        }
        max = min = 0; // treat empty concat as the empty string
        unsigned num = a->get_num_args();
        bool update_min = true;
        bool update_max = true;
        unsigned tmp_min, tmp_max;
        while (num-- && update_max) {
            expr * const curr = a->get_arg(num);
            const unsigned curr_sz = m_util.get_bv_size(curr);
            count_trailing(curr, tmp_min, tmp_max, depth - 1);
            SASSERT(curr_sz != tmp_min || curr_sz == tmp_max);
            max += tmp_max;
            if (update_min) min += tmp_min;
            //  continue updating only if eaten away completely
            update_min &= curr_sz == tmp_min;
            update_max &= curr_sz == tmp_max;
        }
    }

    void count_trailing_add(app * a, unsigned& min, unsigned& max, unsigned depth) {
        if (depth <= 1) {
            min = 0;
            max = m_util.get_bv_size(a);
        }
        const unsigned num = a->get_num_args();
        const unsigned sz = m_util.get_bv_size(a);
        min = max = sz; // treat empty addition as 0
        unsigned tmp_min;
        unsigned tmp_max;
        bool known_parity = true;
        bool is_odd = false;
        for (unsigned i = 0; i < num; ++i) {
            expr * const curr = a->get_arg(i);
            count_trailing(curr, tmp_min, tmp_max, depth - 1);
            min = std::min(min, tmp_min);
            known_parity = known_parity && (!tmp_max || tmp_min);
            if (known_parity && !tmp_max) is_odd = !is_odd;
            if (!known_parity && !min) break; // no more information can be gained
        }
        max = known_parity && is_odd ? 0 : sz; // max is known if parity is 1
    }

    void count_trailing_mul(app * a, unsigned& min, unsigned& max, unsigned depth) {
        if (depth <= 1) {
            min = 0;
            max = m_util.get_bv_size(a);
        }

        const unsigned num = a->get_num_args();
        if (!num) {
            max = min = 0; // treat empty multiplication as 1
            return;
        }
        // assume that numerals are pushed in the front, count only for the first element
        expr * const curr = a->get_arg(0);
        unsigned tmp_max;
        count_trailing(curr, min, tmp_max, depth - 1);
        max = num == 1 ? tmp_max : m_util.get_bv_size(a);
        return;
    }

    void count_trailing_core(expr * e, unsigned& min, unsigned& max, unsigned depth) {
        if (!depth) {
            min = 0;
            max = m_util.get_bv_size(e);
            return;
        }
        unsigned sz;
        numeral e_val;
        if (m_util.is_numeral(e, e_val, sz)) {
            min = max = 0;
            numeral two(2);
            while (sz-- && e_val.is_even()) {
                ++max;
                ++min;
                div(e_val, two, e_val);
            }
            return;
        }
        if (m_util.is_bv_mul(e)) count_trailing_mul(to_app(e), min, max, depth);
        else if (m_util.is_bv_add(e)) count_trailing_add(to_app(e), min, max, depth);
        else if (m_util.is_concat(e)) count_trailing_concat(to_app(e), min, max, depth);
        else {
            min = 0;
            max = m_util.get_bv_size(e);
        }
    }
};

bv_trailing::bv_trailing(mk_extract_proc& mk_extract) {
    m_imp = alloc(imp, mk_extract);
}

bv_trailing::~bv_trailing() {
    if (m_imp) dealloc(m_imp);
}

br_status bv_trailing::eq_remove_trailing(expr * e1, expr * e2,  expr_ref& result) {
    return m_imp->eq_remove_trailing(e1, e2, result);
}

unsigned bv_trailing::remove_trailing(expr * e, unsigned n, expr_ref& result, unsigned depth) {
    return m_imp->remove_trailing(e, n, result, depth);
}

void bv_trailing::reset_cache() {
    m_imp->reset_cache();
}
