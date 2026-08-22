/// - verify: server
///
/// Positions are UTF-16 code units: the string mixes a CJK char, a
/// 2-byte char, and a surrogate-pair char, so the columns after it pin
/// the byte/UTF-16 conversion on both the cursor and the reply side.

int §(def)width = 3;

const char* text = "宽¢€𝄞"; int §(after_unicode)area = §(use)width + 1;
