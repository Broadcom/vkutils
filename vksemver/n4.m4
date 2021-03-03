dnl  n4_define_substrings_as(string, regexp, macro0[, macro1[, ... macroN ]])
dnl  ***************************************************************************
dnl
dnl  Searches for the first match of `regexp` in `string` and defines custom
dnl  macros accordingly
dnl
dnl  For both the entire regular expression `regexp` (`\0`) and each
dnl  sub-expression within capturing parentheses (`\1`, `\2`, `\3`, ... , `\N`)
dnl  a macro expanding to the corresponding matching text will be created,
dnl  named according to the argument `macroN` passed. If a `macroN` argument is
dnl  omitted or empty, the corresponding parentheses in the regular expression
dnl  will be considered as non-capturing. If `regexp` cannot be found in
dnl  `string` no macro will be defined. If `regexp` can be found but some of
dnl  its capturing parentheses cannot, the macro(s) corresponding to the latter
dnl  will be defined as empty strings.
dnl
dnl  Source: https://github.com/madmurphy/not-autotools
dnl
dnl  ***************************************************************************
m4_define([n4_define_substrings_as],
    [m4_bregexp([$1], [$2],
        m4_ifnblank([$3],
            [[m4_define(m4_normalize([$3]), [m4_quote(\&)])]])[]m4_if(m4_eval([$# > 3]), [1],
            [m4_for([_idx_], [4], [$#], [1],
                [m4_ifnblank(m4_quote(m4_argn(_idx_, $@)),
                    [[m4_define(m4_normalize(m4_argn(]_idx_[, $@)), m4_quote(\]m4_eval(_idx_[ - 3])[))]])])]))])
m4_define(n4_define_git_hash, m4_esyscmd_s([[ -d .git ]] && [git describe --abbrev=7 --always --tags]))
