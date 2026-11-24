#include <gtest/gtest.h>

#include <string>

#include "qac/alphabet.hpp"
#include "qac/crc32c.hpp"
#include "qac/text.hpp"
#include "qac/utf8.hpp"

using namespace qac;

namespace {
const Alphabet& real_alphabet() {
  static const Alphabet a = Alphabet::load(std::string(QAC_DATA_DIR) + "/alphabet.txt");
  return a;
}
std::string enc(const Alphabet& a, const std::string& utf8) {
  std::u32string cps;
  EXPECT_TRUE(utf8::decode(utf8, cps));
  std::string out;
  EXPECT_TRUE(a.encode_word(cps, out)) << utf8;
  return out;
}
}  // namespace

TEST(Crc32c, KnownVectors) {
  EXPECT_EQ(crc32c("123456789", 9), 0xE3069283u);
  EXPECT_EQ(crc32c("", 0), 0u);
  const std::string a(1000, 'x');
  const std::string b(777, 'y');
  EXPECT_EQ(crc32c_extend(crc32c(a.data(), a.size()), b.data(), b.size()),
            crc32c((a + b).data(), a.size() + b.size()));
}

TEST(Utf8, RoundTrip) {
  const std::string s = "aЖß€😀";
  std::u32string cps;
  ASSERT_TRUE(utf8::decode(s, cps));
  EXPECT_EQ(cps.size(), 5u);
  EXPECT_EQ(utf8::encode(cps), s);
}

TEST(Utf8, RejectsMalformed) {
  std::u32string out;
  EXPECT_FALSE(utf8::decode(std::string("\xC0\x80"), out));       // overlong NUL
  EXPECT_FALSE(utf8::decode(std::string("\xED\xA0\x80"), out));   // surrogate
  EXPECT_FALSE(utf8::decode(std::string("\xE2\x82"), out));       // truncated
  EXPECT_FALSE(utf8::decode(std::string("\x80"), out));           // stray continuation
  EXPECT_FALSE(utf8::decode(std::string("\xF5\x80\x80\x80"), out));  // > U+10FFFF
  EXPECT_FALSE(utf8::decode(std::string("a\xFF" "b"), out));
  EXPECT_TRUE(utf8::decode(std::string(), out));
}

TEST(Alphabet, ShippedFileLoads) {
  const Alphabet& a = real_alphabet();
  EXPECT_GT(a.size(), 80u);
  EXPECT_LE(a.size(), 255u);
}

TEST(Alphabet, CaseFolding) {
  const Alphabet& a = real_alphabet();
  EXPECT_EQ(enc(a, "Hello"), enc(a, "hELLO"));
  EXPECT_EQ(enc(a, "ПРИВЕТ"), enc(a, "привет"));
  EXPECT_EQ(enc(a, "STRASSE"), enc(a, "strasse"));
  EXPECT_EQ(enc(a, "Ñandú"), enc(a, "ñandú"));
  EXPECT_EQ(enc(a, "ÜBER"), enc(a, "über"));
  EXPECT_EQ(enc(a, "Été"), enc(a, "éTÉ"));
}

TEST(Alphabet, YoFoldsIntoYe) {
  const Alphabet& a = real_alphabet();
  EXPECT_EQ(enc(a, "ёж"), enc(a, "еж"));
  EXPECT_EQ(enc(a, "Ёлка"), enc(a, "елка"));
}

TEST(Alphabet, AccentsStayDistinct) {
  const Alphabet& a = real_alphabet();
  EXPECT_NE(enc(a, "uber"), enc(a, "über"));
  EXPECT_NE(enc(a, "cafe"), enc(a, "café"));
}

TEST(Alphabet, DecodeUsesCanonicalLowercase) {
  const Alphabet& a = real_alphabet();
  EXPECT_EQ(a.decode_word(enc(a, "ПриВет")), "привет");
  EXPECT_EQ(a.decode_word(enc(a, "Ёж")), "ее" == std::string("ее") ? "еж" : "");  // ё -> е
  EXPECT_EQ(a.decode_word(enc(a, "Don’t")), "don't");
  EXPECT_EQ(a.decode_word(enc(a, "ẞ")), "ß");
}

TEST(Alphabet, DigitsAreLetters) {
  const Alphabet& a = real_alphabet();
  EXPECT_NE(a.code(U'7'), 0);
  EXPECT_NE(enc(a, "iphone15"), "");
}

TEST(Alphabet, ForeignCharactersAreRejected) {
  const Alphabet& a = real_alphabet();
  std::string out;
  EXPECT_FALSE(a.encode_word(U"日本", out));
  EXPECT_FALSE(a.encode_word(U"a😀b", out));
  EXPECT_TRUE(out.empty());
}

TEST(Alphabet, FingerprintIsStableAndSensitive) {
  const auto a1 = Alphabet::from_string("a A\nb B\n");
  const auto a2 = Alphabet::from_string("a A\nb B\n");
  const auto a3 = Alphabet::from_string("a A\nb B\nc C\n");
  const auto a4 = Alphabet::from_string("a A\nb B\n@separators .\n");
  const auto a5 = Alphabet::from_string("b B\na A\n");  // different codes
  EXPECT_EQ(a1.fingerprint(), a2.fingerprint());
  EXPECT_NE(a1.fingerprint(), a3.fingerprint());
  EXPECT_NE(a1.fingerprint(), a4.fingerprint());
  EXPECT_NE(a1.fingerprint(), a5.fingerprint());
}

TEST(Alphabet, FormatErrors) {
  EXPECT_THROW(Alphabet::from_string(""), std::runtime_error);
  EXPECT_THROW(Alphabet::from_string("# only a comment\n"), std::runtime_error);
  EXPECT_THROW(Alphabet::from_string("a A\nA B\n"), std::runtime_error);     // A twice
  EXPECT_THROW(Alphabet::from_string("ab\n"), std::runtime_error);           // two chars
  EXPECT_THROW(Alphabet::from_string("a A\n@separators a\n"), std::runtime_error);
  EXPECT_THROW(Alphabet::from_string("a A\n@bogus x\n"), std::runtime_error);
  EXPECT_THROW(Alphabet::from_string("a U+ZZZ\n"), std::runtime_error);
  EXPECT_THROW(Alphabet::from_string("a U+D800\n"), std::runtime_error);     // surrogate
  EXPECT_THROW(Alphabet::from_string("a U+0020\n"), std::runtime_error);     // whitespace
  EXPECT_THROW(Alphabet::from_string("a \xFF\n"), std::runtime_error);       // bad UTF-8
}

TEST(Alphabet, ErrorMessagesNameTheLine) {
  try {
    Alphabet::from_string("a A\nb B\nab\n", "my.txt");
    FAIL();
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("my.txt:3"), std::string::npos) << e.what();
  }
}

TEST(Alphabet, LimitOf255Letters) {
  std::string text;
  for (int i = 0; i < 255; ++i) {
    std::string ch;
    utf8::append(static_cast<char32_t>(0x4E00 + i), ch);
    text += ch + "\n";
  }
  EXPECT_EQ(Alphabet::from_string(text).size(), 255u);
  std::string ch;
  utf8::append(static_cast<char32_t>(0x4E00 + 255), ch);
  EXPECT_THROW(Alphabet::from_string(text + ch + "\n"), std::runtime_error);
}

TEST(Alphabet, CommentsAndBom) {
  const auto a = Alphabet::from_string("\xEF\xBB\xBF# header\na A  # trailing\n\n  b B\r\n");
  EXPECT_EQ(a.size(), 2u);
}

// ------------------------------------------------------------------ TextProcessor

class TextTest : public ::testing::Test {
 protected:
  TextProcessor tp{real_alphabet()};
  std::vector<std::string> words;
  std::string partial;
  std::string detail;

  std::string shown() const { return tp.render(words); }
};

TEST_F(TextTest, SplitsAndNormalizes) {
  ASSERT_EQ(tp.parse_query("  New   York,\tWEATHER!  ", words), ParseError::None);
  EXPECT_EQ(shown(), "new york weather");
}

TEST_F(TextTest, MultilingualQuery) {
  ASSERT_EQ(tp.parse_query("Привет, мир! Straße ñandú", words), ParseError::None);
  EXPECT_EQ(shown(), "привет мир straße ñandú");
}

TEST_F(TextTest, PunctuationSeparatesButApostropheDoesNot) {
  ASSERT_EQ(tp.parse_query("don't e-mail (home)", words), ParseError::None);
  EXPECT_EQ(shown(), "don't e mail home");
}

TEST_F(TextTest, EmptyAndSeparatorOnlyQueriesAreRejectedForStorage) {
  EXPECT_EQ(tp.parse_query("", words), ParseError::Empty);
  EXPECT_EQ(tp.parse_query("   \t ", words), ParseError::Empty);
  EXPECT_EQ(tp.parse_query("...!!!", words), ParseError::Empty);
}

TEST_F(TextTest, UnknownCharacterRejectsWholeQuery) {
  EXPECT_EQ(tp.parse_query("good 日本語 query", words, &detail), ParseError::UnknownCharacter);
  EXPECT_TRUE(words.empty());
  EXPECT_NE(detail.find("日"), std::string::npos) << detail;
  EXPECT_EQ(tp.parse_query("emoji 😀", words), ParseError::UnknownCharacter);
}

TEST_F(TextTest, InvalidUtf8) {
  EXPECT_EQ(tp.parse_query(std::string("ab\xFF" "cd"), words), ParseError::InvalidUtf8);
}

TEST_F(TextTest, Limits) {
  Limits lim;
  lim.max_words = 3;
  lim.max_word_letters = 5;
  lim.max_input_bytes = 40;
  TextProcessor small(real_alphabet(), lim);
  EXPECT_EQ(small.parse_query("one two three", words), ParseError::None);
  EXPECT_EQ(small.parse_query("one two three four", words), ParseError::TooManyWords);
  EXPECT_EQ(small.parse_query("abcdef", words), ParseError::TooLong);
  EXPECT_EQ(small.parse_query(std::string(41, 'a'), words), ParseError::TooLong);
}

TEST_F(TextTest, PrefixWithPartialWord) {
  ASSERT_EQ(tp.parse_prefix("New Yo", words, partial), ParseError::None);
  EXPECT_EQ(shown(), "new");
  EXPECT_EQ(tp.alphabet().decode_word(partial), "yo");
}

TEST_F(TextTest, PrefixWithTrailingSpaceHasNoPartial) {
  ASSERT_EQ(tp.parse_prefix("new york ", words, partial), ParseError::None);
  EXPECT_EQ(shown(), "new york");
  EXPECT_TRUE(partial.empty());
  ASSERT_EQ(tp.parse_prefix("new york,", words, partial), ParseError::None);  // separator counts too
  EXPECT_TRUE(partial.empty());
  EXPECT_EQ(words.size(), 2u);
}

TEST_F(TextTest, PrefixSingleWordIsPartial) {
  ASSERT_EQ(tp.parse_prefix("hel", words, partial), ParseError::None);
  EXPECT_TRUE(words.empty());
  EXPECT_EQ(tp.alphabet().decode_word(partial), "hel");
}

TEST_F(TextTest, PrefixEmptyIsValid) {
  ASSERT_EQ(tp.parse_prefix("", words, partial), ParseError::None);
  EXPECT_TRUE(words.empty());
  EXPECT_TRUE(partial.empty());
  ASSERT_EQ(tp.parse_prefix("   ", words, partial), ParseError::None);
  EXPECT_TRUE(words.empty());
}

TEST_F(TextTest, PrefixRejectsForeignCharacters) {
  EXPECT_EQ(tp.parse_prefix("new 日", words, partial), ParseError::UnknownCharacter);
  EXPECT_TRUE(words.empty());
}
