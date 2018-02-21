#pragma once
#ifndef SIG_TREE_LIKELY_H
#define SIG_TREE_LIKELY_H

#define SGT_LIKELY(x)   (__builtin_expect((x), 1))
#define SGT_UNLIKELY(x) (__builtin_expect((x), 0))

#endif //SIG_TREE_LIKELY_H
