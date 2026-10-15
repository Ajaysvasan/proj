package proj.agent.rt;

/** Minimal JSON output (the agent has no dependencies) and the two lookups it reads back. */
final class Json {
  private Json() {}

  static StringBuilder str(StringBuilder sb, String s) {
    if (s == null) return sb.append("null");
    sb.append('"');
    for (int i = 0; i < s.length(); i++) {
      char c = s.charAt(i);
      switch (c) {
        case '"':
          sb.append("\\\"");
          break;
        case '\\':
          sb.append("\\\\");
          break;
        case '\n':
          sb.append("\\n");
          break;
        case '\r':
          sb.append("\\r");
          break;
        case '\t':
          sb.append("\\t");
          break;
        default:
          if (c < 0x20 || (c >= 0xD800 && c <= 0xDFFF && !validSurrogate(s, i))) {
            sb.append(String.format("\\u%04x", (int) (c < 0x20 ? c : 0xFFFD)));
          } else {
            sb.append(c);
          }
      }
    }
    return sb.append('"');
  }

  private static boolean validSurrogate(String s, int i) {
    char c = s.charAt(i);
    if (Character.isHighSurrogate(c)) return i + 1 < s.length() && Character.isLowSurrogate(s.charAt(i + 1));
    return i > 0 && Character.isHighSurrogate(s.charAt(i - 1));
  }

  /** Value of a string or number field in a small flat JSON object, or null. */
  static String field(String json, String key) {
    String k = "\"" + key + "\"";
    int at = json.indexOf(k);
    if (at < 0) return null;
    int colon = json.indexOf(':', at + k.length());
    if (colon < 0) return null;
    int i = colon + 1;
    while (i < json.length() && Character.isWhitespace(json.charAt(i))) i++;
    if (i >= json.length()) return null;
    if (json.charAt(i) == '"') {
      int end = json.indexOf('"', i + 1);
      return end < 0 ? null : json.substring(i + 1, end);
    }
    int end = i;
    while (end < json.length() && ",}] \n\r\t".indexOf(json.charAt(end)) < 0) end++;
    return json.substring(i, end);
  }
}
