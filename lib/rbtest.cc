#define INTERVAL_TREE_DEBUG 1
#define rbaccount(x) ++rbaccount_##x
unsigned long long rbaccount_rotation, rbaccount_flip, rbaccount_insert, rbaccount_erase;
#include <iostream>
#include <string.h>
#include <boost/intrusive/set.hpp>
#include <boost/random.hpp>
#include "rb.hh"
#include "interval.hh"
#include "interval_tree.hh"
#include "str.hh"
#include <sys/time.h>
#include <sys/resource.h>
static bool print_actions;

#if 0
// archive working iterative version of delete
template <typename T, typename C, typename R>
T* rbtree<T, C, R>::delete_node(T* victim) {
    // construct path to root
    local_stack<T*, (sizeof(size_t) << 2)> stk;
    for (T* n = victim; n; n = n->rblinks_.p_)
        stk.push(n);

    // work backwards
    int si = stk.size() - 1;
    rbnodeptr<T> np(stk[si], false), repl;
    size_t childtrack = 0, redtrack = 0;
    while (1) {
        bool direction;
        if (si && np.child(false).node() == stk[si-1]) {
            if (!np.child(false).red() && !np.child(false).child(false).red())
                np = np.move_red_left(r_.reshape());
            direction = false;
        } else {
            if (np.child(false).red())
                np = np.rotate_right(r_.reshape());
            if (victim == np.node() && !np.child(true)) {
                repl = rbnodeptr<T>(0, false);
                break;
            }
            if (!np.child(true).red() && !np.child(true).child(false).red())
                np = np.move_red_right(r_.reshape());
            if (victim == np.node()) {
                T* min;
                np.child(true) = unlink_min(np.child(true), &min);
                min->rblinks_ = np.node()->rblinks_;
                for (int i = 0; i < 2; ++i)
                    if (min->rblinks_.c_[i])
                        min->rblinks_.c_[i].parent() = min;
                repl = rbnodeptr<T>(min, np.red()).fixup(r_.reshape());
                break;
            }
            direction = true;
        }
        childtrack = (childtrack << 1) | direction;
        redtrack = (redtrack << 1) | np.red();
        np = np.child(direction);
        if (np.node() != stk[si])
            --si;
    }

    // now work up
    if (T* p = np.parent())
        do {
            p->rblinks_.c_[childtrack & 1] = repl;
            repl = rbnodeptr<T>(p, redtrack & 1);
            repl = repl.fixup(r_.reshape());
            childtrack >>= 1;
            redtrack >>= 1;
            p = repl.parent();
        } while (p);
    return repl.node();
}
#endif

template <typename T>
class rbwrapper : public T {
  public:
    template <typename... Args> inline rbwrapper(Args&&... args)
	: T(std::forward<Args>(args)...) {
    }
    inline rbwrapper(const T& x)
	: T(x) {
    }
    inline rbwrapper(T&& x) noexcept
	: T(std::move(x)) {
    }
    inline const T& value() const {
	return *this;
    }
    rblinks<rbwrapper<T> > rblinks_;
};

template <> class rbwrapper<int> {
  public:
    template <typename... Args> inline rbwrapper(int x)
	: x_(x) {
    }
    inline int value() const {
	return x_;
    }
    int x_;
    rblinks<rbwrapper<int> > rblinks_;
};

std::ostream& operator<<(std::ostream& s, rbwrapper<int> x) {
    return s << x.value();
}

template <typename T>
class default_comparator<rbwrapper<T> > {
  public:
    inline int operator()(const rbwrapper<T> &a, const rbwrapper<T> &b) const {
	return default_compare(a.value(), b.value());
    }
    inline int operator()(const rbwrapper<T> &a, const T &b) const {
	return default_compare(a.value(), b);
    }
    inline int operator()(const T &a, const rbwrapper<T> &b) const {
	return default_compare(a, b.value());
    }
    inline int operator()(const T &a, const T &b) const {
	return default_compare(a, b);
    }
};

template <typename T> class less {};
template <typename T>
class less<rbwrapper<T> > {
  public:
    inline bool operator()(const rbwrapper<T>& a, const rbwrapper<T>& b) const {
	return a.value() < b.value();
    }
    inline bool operator()(const rbwrapper<T>& a, const T& b) const {
	return a.value() < b;
    }
    inline bool operator()(const T& a, const rbwrapper<T>& b) const {
	return a < b.value();
    }
    inline bool operator()(const T& a, const T& b) const {
	return a < b;
    }
};

void print(interval<int> &x) {
    std::cerr << x << "\n";
}

template <typename A, typename B>
struct semipair {
    A first;
    B second;
    inline semipair() {
    }
    inline semipair(const A &a)
	: first(a) {
    }
    inline semipair(const A &a, const B &b)
	: first(a), second(b) {
    }
    inline semipair(const semipair<A, B> &x)
	: first(x.first), second(x.second) {
    }
    template <typename X, typename Y> inline semipair(const semipair<X, Y> &x)
	: first(x.first), second(x.second) {
    }
    template <typename X, typename Y> inline semipair(const std::pair<X, Y> &x)
	: first(x.first), second(x.second) {
    }
    inline semipair<A, B> &operator=(const semipair<A, B> &x) {
	first = x.first;
	second = x.second;
	return *this;
    }
    template <typename X, typename Y>
    inline semipair<A, B> &operator=(const semipair<X, Y> &x) {
	first = x.first;
	second = x.second;
	return *this;
    }
    template <typename X, typename Y>
    inline semipair<A, B> &operator=(const std::pair<X, Y> &x) {
	first = x.first;
	second = x.second;
	return *this;
    }
};

template <typename A, typename B>
std::ostream &operator<<(std::ostream &s, const semipair<A, B> &x) {
    return s << '<' << x.first << ", " << x.second << '>';
}

struct compare_first {
    template <typename T, typename U>
    inline int operator()(const T &a, const std::pair<T, U> &b) const {
	return default_compare(a, b.first);
    }
    template <typename T, typename U, typename V>
    inline int operator()(const std::pair<T, U> &a, const std::pair<T, V> &b) const {
	return default_compare(a.first, b.first);
    }
    template <typename T, typename U>
    inline int operator()(const T &a, const semipair<T, U> &b) const {
	return default_compare(a, b.first);
    }
    template <typename T, typename U, typename V>
    inline int operator()(const semipair<T, U> &a, const semipair<T, V> &b) const {
	return default_compare(a.first, b.first);
    }
};

template <typename T>
struct rbinterval : public interval<T> {
    T subtree_iend_;
    rbinterval(const interval<T>& x)
	: interval<T>(x), subtree_iend_(x.iend()) {
    }
    rbinterval(T first, T last)
	: interval<T>(first, last), subtree_iend_(this->iend()) {
    }
    T subtree_iend() const {
	return subtree_iend_;
    }
    void set_subtree_iend(T x) {
	subtree_iend_ = x;
    }
};

typedef rbinterval<int> int_interval;
typedef rbinterval<Str> str_interval;

template <typename T>
std::ostream& operator<<(std::ostream& s, const rbwrapper<rbinterval<T> >& x) {
    return s << "[" << x.ibegin() << ", " << x.iend() << ") ..." << x.subtree_iend();
}

void rbaccount_report() {
    unsigned long long all = rbaccount_insert + rbaccount_erase;
    fprintf(stderr, "{\"insert\":%llu,\"erase\":%llu,\"rotation_per_operation\":%g,\"flip_per_operation\":%g}\n",
            rbaccount_insert, rbaccount_erase, (double) rbaccount_rotation / all, (double) rbaccount_flip / all);
}

template <typename G>
void grow_and_shrink(G& tree, int N) {
    struct rusage ru[6];
    int *x = new int[N];
    for (int i = 0; i < N; ++i)
        x[i] = i;
    getrusage(RUSAGE_SELF, &ru[0]);
    for (int i = 0; i < N; ++i) {
        int j = random() % (N - i);
        int val = x[j];
        x[j] = x[N - i - 1];
        tree.insert(val);
    }
    getrusage(RUSAGE_SELF, &ru[1]);
    tree.phase(1);
    getrusage(RUSAGE_SELF, &ru[2]);
    int count = 0;
    for (int i = 0; i < 4 * N; ++i)
        count += tree.contains(random() % N);
    getrusage(RUSAGE_SELF, &ru[3]);
    tree.phase(2);
    for (int i = 0; i < N; ++i)
        x[i] = i;
    getrusage(RUSAGE_SELF, &ru[4]);
    for (int i = 0; i < N; ++i) {
        int j = random() % (N - i);
        int val = x[j];
        x[j] = x[N - i - 1];
        tree.find_and_erase(val);
        //if (i % 1000 == 999) std::cerr << "\n\n" << i << "\n" << tree << "\n\n";
    }
    getrusage(RUSAGE_SELF, &ru[5]);
    tree.phase(3);
    delete[] x;
    for (int i = 5; i > 0; --i)
        timersub(&ru[i].ru_utime, &ru[i-1].ru_utime, &ru[i].ru_utime);
    fprintf(stderr, "time: insert %ld.%06ld  find %ld.%06ld  remove %ld.%06ld  count %d\n",
            long(ru[1].ru_utime.tv_sec), long(ru[1].ru_utime.tv_usec),
            long(ru[3].ru_utime.tv_sec), long(ru[3].ru_utime.tv_usec),
            long(ru[5].ru_utime.tv_sec), long(ru[5].ru_utime.tv_usec),
            count);
}

template <typename G>
void fuzz(G& tree, int N) {
    boost::mt19937 gen;
    boost::random_number_generator<boost::mt19937> rng(gen);
    const int SZ = 5000;
    int in[SZ];
    memset(in, 0, sizeof(in));
    for (int i = 0; i < N; ++i) {
        int op = rng(8), which = rng(SZ);
        if (op < 5) {
            if (print_actions)
                std::cerr << "find " << which << "\n";
            bool c = tree.contains(which);
            assert(c ? in[which] : !in[which]);
        } else if (!in[which] || (op == 5 && in[which] < 4)) {
            if (print_actions)
                std::cerr << "insert " << which << "\n";
            assert(!!in[which] == tree.contains(which));
            tree.insert(which);
            ++in[which];
        } else {
            if (print_actions)
                std::cerr << "erase " << which << "\n";
            assert(tree.contains(which));
            tree.find_and_erase(which);
            --in[which];
        }
        tree.phase(0);
        if (i % 1000 == 0 && i && !print_actions)
            std::cerr << ".";
    }
}

struct rbtree_with_print {
    rbtree<rbwrapper<int> > tree;
    inline void insert(int val) {
        tree.insert(*new rbwrapper<int>(val));
    }
    inline void find_and_erase(int val) {
        tree.erase_and_dispose(*tree.find(rbwrapper<int>(val)));
    }
    inline void find(int) {
    }
    inline void phase(int ph) {
        if (ph != 2)
            std::cerr << tree << "\n\n";
        else {
            for (auto it = tree.lower_bound(rbwrapper<int>(5)); it != tree.lower_bound(rbwrapper<int>(10)); ++it)
                std::cerr << it->value() << "\n";
        }
    }
};

struct rbtree_without_print {
    rbtree<rbwrapper<int> > tree;
    inline void insert(int val) {
	//std::cerr << "insert " << val << "\n";
        tree.insert(*new rbwrapper<int>(val));
	//tree.check();
    }
    inline void find_and_erase(int val) {
	//std::cerr << "erase " << val << "\n" << tree << "\n";
        tree.erase_and_dispose(*tree.find(rbwrapper<int>(val)));
	//tree.check();
    }
    inline bool contains(int val) {
        return tree.find(rbwrapper<int>(val)).operator->() != 0;
    }
    inline void phase(int) {
        tree.check();
        //std::cout << tree << "\n";
    }
};

namespace bi = boost::intrusive;
struct boost_set_without_print {
    struct node : public bi::set_base_hook<bi::optimize_size<true> > {
        int value;
        node(int x)
            : value(x) {
        }
        bool operator<(const node& x) const {
            return value < x.value;
        }
    };
    struct node_comparator {
        bool operator()(int a, const node& b) {
            return a < b.value;
        }
        bool operator()(const node& a, int b) {
            return a.value < b;
        }
    };
    struct node_disposer {
        void operator()(node* n) {
            delete n;
        }
    };
    bi::set<node> tree;
    inline void insert(int val) {
        tree.insert(*new node(val));
    }
    inline void find_and_erase(int val) {
        tree.erase_and_dispose(tree.find(node(val)), node_disposer());
    }
    inline bool contains(int val) {
        return tree.find(node(val)) != tree.end();
    }
    inline void phase(int) {
    }
};

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-b") == 0) {
        boost_set_without_print tree;
        grow_and_shrink(tree, 1000000);
        exit(0);
    } else if (argc > 1 && strcmp(argv[1], "-p") == 0) {
        rbtree_without_print tree;
        grow_and_shrink(tree, 1000000);
        rbaccount_report();
        exit(0);
    } else if (argc > 1 && strcmp(argv[1], "-f") == 0) {
        rbtree_without_print tree;
        fuzz(tree, 1000000);
        rbaccount_report();
        exit(0);
    } else if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        {
            rbtree_without_print tree;
            int N = 1000000;
            int *x = new int[N];
            for (int i = 0; i < N; ++i)
                x[i] = i;
            for (int i = 0; i < N; ++i) {
                int j = random() % (N - i);
                int val = x[j];
                x[j] = x[N - i - 1];
                tree.insert(val);
            }
            tree.tree.check();
            while (auto item = tree.tree.unlink_leftmost_without_rebalance())
                delete item;
            delete[] x;
        }
        exit(0);
    } else if (argc > 1) {
        fprintf(stderr, "Usage: ./a.out [-b|-p|-f|-d]\n");
        exit(1);
    } else {
        rbtree_with_print tree;
        //grow_and_shrink(tree, 50000);
    }

    {
	rbtree<rbwrapper<int> > tree;
	tree.insert(*new rbwrapper<int>(0));
	auto x = new rbwrapper<int>(1);
	tree.insert(*x);
	tree.insert(*new rbwrapper<int>(0));
	tree.insert(*new rbwrapper<int>(-2));
	auto y = new rbwrapper<int>(0);
	tree.insert(*y);
	std::cerr << tree << "\n";
	tree.erase_and_dispose(*x);
	std::cerr << tree << "\n";
	tree.erase_and_dispose(*y);
	std::cerr << tree << "\n";
    }

    {
        rbtree<rbwrapper<int> > tree;
        tree.insert(*new rbwrapper<int>(0));
        tree.insert(*new rbwrapper<int>(2));
        for (int i = 0; i != 40; ++i)
            tree.insert(*new rbwrapper<int>(1));
        auto it = tree.lower_bound(1, default_comparator<rbwrapper<int>>());
        assert(it->value() == 1);
        --it;
        assert(it->value() == 0);
        it = tree.upper_bound(1, default_comparator<rbwrapper<int>>());
        assert(it->value() == 2);
        it = tree.lower_bound(1, less<rbwrapper<int>>());
        assert(it->value() == 1);
        --it;
        assert(it->value() == 0);
        it = tree.upper_bound(1, less<rbwrapper<int>>());
        assert(it->value() == 2);
    }

    {
        interval_tree<rbwrapper<int_interval> > tree;
        for (int i = 0; i < 100; ++i) {
            int a = random() % 1000;
            tree.insert(*new rbwrapper<int_interval>(a, a + random() % 200));
        }
        std::cerr << tree << "\n\n";
        for (auto it = tree.begin_contains(40); it != tree.end(); ++it)
            std::cerr << "... " << *it << "\n";
        std::cerr << "\n";
        for (auto it = tree.begin_overlaps(interval<int>(10, 30)); it != tree.end(); ++it)
            std::cerr << "... " << *it << "\n";
    }

    {
        interval_tree<rbwrapper<str_interval> > tree;
        tree.insert(*new rbwrapper<str_interval>("t|", "t}"));
        tree.insert(*new rbwrapper<str_interval>("t|00001|0000000001", "t|00001}"));
        std::cerr << tree << "\n\n";
        tree.insert(*new rbwrapper<str_interval>("t|00001|0000000001", "t|00001}"));
        std::cerr << tree << "\n\n";
    }
}
