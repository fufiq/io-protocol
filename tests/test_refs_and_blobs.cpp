// test_refs_and_blobs.cpp - a reference that knows its target, and a payload
// whose length is not decided at compile time.
//
// Half of what ref<T> is for cannot be tested by running anything: the point is
// that certain code does NOT compile. Those are `requires` expressions, which
// ask the compiler the question and let the test fail loudly if the answer ever
// changes - a comment claiming "this would not compile" is worth nothing.

#include <doctest/doctest.h>

#include "../storage/blob.hpp"
#include "../storage/indexes.hpp"

#include <cstdio>
#include <string>
#include <vector>

using storage::record_id;
using storage::tag_t;

namespace {

    enum class kind : tag_t { customer = 1, product = 2, order_line = 3, text = 4 };

    struct customer {
        static constexpr tag_t tag = (tag_t)kind::customer;
        uint64_t email_hash;
        int64_t  balance;
    };

    struct product {
        static constexpr tag_t tag = (tag_t)kind::product;
        uint64_t sku_hash;
        int64_t  price;
    };

    using text_ref = storage::blob_ref<(tag_t)kind::text>;

    // The shape app_layer.hpp has today, with the comments turned into types.
    struct order_line {
        static constexpr tag_t tag = (tag_t)kind::order_line;
        storage::ref<customer> buyer;
        storage::ref<product>  item;
        text_ref               note;
        int64_t                qty;
    };

    static_assert(storage::tagged_aggregate<customer>);
    static_assert(storage::tagged_aggregate<product>);
    static_assert(storage::tagged_aggregate<order_line>);

    // The whole claim about cost: a typed reference is the same eight bytes the
    // raw id was, so nothing about the record's layout moved.
    static_assert(sizeof(storage::ref<customer>) == sizeof(record_id));
    static_assert(sizeof(text_ref) == sizeof(record_id));
    static_assert(sizeof(order_line) == 4 * sizeof(uint64_t));

    using spec = storage::field_spec<"t_ref.log", 1u << 22, "t_ref.idx", 8192, 8192, 8, 4096>;
    using indexes = storage::index_set<storage::multi_index<&order_line::buyer>>;
    using table_t = storage::field_storage<spec, indexes>;

    // Named concepts rather than a bare `!requires(...)` in the test body: with
    // concrete types the expression is non-dependent, and a non-dependent
    // invalid expression is a hard error rather than a false requires-clause.
    // Naming it puts a template parameter in the way, which is what makes the
    // question askable at all.
    template <typename A, typename B>
    concept comparable = requires(A a, B b) { a == b; };

    template <typename table, typename target, typename R>
    concept readable_as = requires(table& t, R r) { t.template get<target>(r); };

    void wipe() { std::remove(spec::path.value); std::remove(spec::index_path); }

    struct fixture {
        fixture() { wipe(); table = new table_t(); REQUIRE(table->ok()); }
        ~fixture() { delete table; wipe(); }
        table_t* table = nullptr;
    };

} // namespace

TEST_CASE("a typed reference reads back its own target without being told the type") {
    fixture f;
    const auto buyer = f.table->create_ref(customer{ 1111, 500 });
    const auto item = f.table->create_ref(product{ 2222, 99 });

    auto c = f.table->get(buyer);      // no <customer> anywhere
    auto p = f.table->get(item);
    REQUIRE(c.has_value());
    REQUIRE(p.has_value());
    CHECK(c->email_hash == 1111);
    CHECK(p->price == 99);
    CHECK(f.table->alive(buyer));
}

TEST_CASE("the wrong target does not compile") {
    // This is the half of ref<T> that has no runtime behaviour to check, so it
    // is checked by asking the compiler directly.
    using ref_customer = storage::ref<customer>;
    using ref_product = storage::ref<product>;

    static_assert(!std::is_convertible_v<ref_customer, ref_product>,
                  "a reference to one type must not convert to a reference to another");
    static_assert(!std::is_convertible_v<record_id, ref_customer>,
                  "a bare id must not become a typed reference by accident - the constructor is explicit");
    static_assert(!std::is_convertible_v<ref_customer, record_id>,
                  "and a typed reference must not decay back to a number silently");

    // get(ref<customer>) must not accept a ref<product>.
    static_assert(readable_as<table_t, customer, ref_customer>);
    static_assert(!readable_as<table_t, customer, ref_product>);

    // Two references of different targets must not compare equal - a swapped
    // pair of arguments is exactly what this catches.
    static_assert(comparable<ref_customer, ref_customer>);
    static_assert(!comparable<ref_customer, ref_product>);
    CHECK(true);   // so the case reports as run rather than empty
}

TEST_CASE("an empty reference is falsy and reads as nothing") {
    fixture f;
    storage::ref<customer> nowhere;
    CHECK(!nowhere);
    CHECK(nowhere.id() == 0);
    CHECK(!f.table->get(nowhere).has_value());
    CHECK(!f.table->alive(nowhere));
}

TEST_CASE("a reference works as an index key, because it is an id wearing a type") {
    fixture f;
    const auto buyer = f.table->create_ref(customer{ 1111, 500 });
    const auto other = f.table->create_ref(customer{ 2222, 700 });
    const auto item = f.table->create_ref(product{ 3333, 99 });

    for (int i = 0; i < 3; ++i) f.table->create(order_line{ buyer, item, {}, i + 1 });
    f.table->create(order_line{ other, item, {}, 9 });

    auto& ix = f.table->observer_state();
    CHECK(ix.count<&order_line::buyer>(buyer) == 3);
    CHECK(ix.count<&order_line::buyer>(other) == 1);

    int64_t qty = 0;
    ix.each<&order_line::buyer>(buyer, [&](record_id id) {
        if (auto l = f.table->get<order_line>(id)) qty += l->qty;
    });
    CHECK(qty == 1 + 2 + 3);
}

TEST_CASE("text of any length lives beside the record that points at it") {
    fixture f;
    const std::string tiny = "x";
    const std::string big(3000, 'q');

    const auto a = storage::put_text<(tag_t)kind::text>(*f.table, tiny);
    const auto b = storage::put_text<(tag_t)kind::text>(*f.table, big);
    const auto empty = storage::put_text<(tag_t)kind::text>(*f.table, "");

    CHECK(storage::blob_size(*f.table, a) == tiny.size());
    CHECK(storage::blob_size(*f.table, b) == big.size());
    CHECK(storage::load_text(*f.table, a) == tiny);
    CHECK(storage::load_text(*f.table, b) == big);
    CHECK(storage::load_text(*f.table, empty).empty());

    // And a record holding one.
    const auto buyer = f.table->create_ref(customer{ 1, 1 });
    const auto item = f.table->create_ref(product{ 2, 2 });
    const auto line = f.table->create(order_line{ buyer, item, b, 5 });

    auto got = f.table->get<order_line>(line);
    REQUIRE(got.has_value());
    CHECK(storage::load_text(*f.table, got->note) == big);
    CHECK(got->note == b);
}

TEST_CASE("a payload past the spec's ceiling is refused, not truncated") {
    // max_record_bytes is a compile-time bound. Honouring half a string would
    // read back as valid, which is worse than refusing it.
    fixture f;
    const std::string too_big(spec::max_record_bytes + 1, 'z');
    const auto r = storage::put_text<(tag_t)kind::text>(*f.table, too_big);
    CHECK(!r);
    CHECK(storage::blob_size(*f.table, r) == 0);
    CHECK(storage::load_text(*f.table, r).empty());
}

TEST_CASE("loading into a caller's buffer never reports a partial copy") {
    fixture f;
    const std::string text = "the quick brown fox";
    const auto r = storage::put_text<(tag_t)kind::text>(*f.table, text);

    std::vector<std::byte> big(64), small(4);
    CHECK(storage::load_blob_into(*f.table, r, big) == text.size());
    CHECK(storage::load_blob_into(*f.table, r, small) == 0);   // refused, not truncated
    CHECK(std::string((const char*)big.data(), text.size()) == text);
}

TEST_CASE("replacing text points the field at new bytes and leaves the old ones to compaction") {
    fixture f;
    const auto buyer = f.table->create_ref(customer{ 1, 1 });
    const auto item = f.table->create_ref(product{ 2, 2 });
    const auto first = storage::put_text<(tag_t)kind::text>(*f.table, "before");
    const auto line = f.table->create(order_line{ buyer, item, first, 1 });

    const auto second = storage::replace_text(*f.table, first, "after, and rather longer than before");
    f.table->update<&order_line::note>(line, second);

    auto got = f.table->get<order_line>(line);
    REQUIRE(got.has_value());
    CHECK(storage::load_text(*f.table, got->note) == "after, and rather longer than before");
    // The old record is still there until compaction - an append-only log does
    // not take anything back, and a blob is not an exception.
    CHECK(storage::load_text(*f.table, first) == "before");
}

TEST_CASE("references and blobs survive a replay") {
    wipe();
    record_id line = 0;
    storage::ref<customer> buyer;
    {
        table_t t;
        REQUIRE(t.ok());
        buyer = t.create_ref(customer{ 1111, 500 });
        const auto item = t.create_ref(product{ 2222, 99 });
        const auto note = storage::put_text<(tag_t)kind::text>(t, std::string(500, 'w'));
        line = t.create(order_line{ buyer, item, note, 7 });
        t.sync();
    }
    {
        table_t t;
        REQUIRE(t.ok());
        t.recover(1u << 22);
        auto got = t.get<order_line>(line);
        REQUIRE(got.has_value());
        CHECK(got->buyer == buyer);
        CHECK(got->qty == 7);
        CHECK(storage::load_text(t, got->note) == std::string(500, 'w'));
        CHECK(t.observer_state().count<&order_line::buyer>(buyer) == 1);
    }
    wipe();
}
