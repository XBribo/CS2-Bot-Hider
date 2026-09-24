using System.Globalization;
using System.Text;
using BotHiderApi;
namespace BotHiderImpl;
internal static class PersonaName {
    internal static string Normalize(string? source)
    {
        if (string.IsNullOrWhiteSpace(source))
            return string.Empty;

        var visibleElements = new List<string>();
        var elements = StringInfo.GetTextElementEnumerator(source);
        while (elements.MoveNext())
        {
            string element = elements.GetTextElement();
            if (!IsInvisiblePersonaElement(element))
                visibleElements.Add(element);
        }

        int first = 0;
        while (first < visibleElements.Count &&
               IsWhitespacePersonaElement(visibleElements[first]))
        {
            first++;
        }

        int last = visibleElements.Count;
        while (last > first && IsWhitespacePersonaElement(visibleElements[last - 1]))
            last--;

        var boundedElements = new List<string>();
        int utf8Bytes = 0;
        for (int index = first; index < last; index++)
        {
            string element = visibleElements[index];
            int elementBytes = Encoding.UTF8.GetByteCount(element);
            if (utf8Bytes + elementBytes > BotHiderContract.MaxPlayerNameUtf8Bytes)
                break;
            boundedElements.Add(element);
            utf8Bytes += elementBytes;
        }

        while (boundedElements.Count > 0 &&
               IsWhitespacePersonaElement(boundedElements[^1]))
        {
            boundedElements.RemoveAt(boundedElements.Count - 1);
        }
        return string.Concat(boundedElements);
    }

    // Returns whether a text element contains only invisible runes
    private static bool IsInvisiblePersonaElement(string element)
    {
        foreach (Rune rune in element.EnumerateRunes())
        {
            if (!IsInvisiblePersonaRune(rune))
                return false;
        }
        return true;
    }

    // Returns whether a text element contains only whitespace and invisible runes
    private static bool IsWhitespacePersonaElement(string element)
    {
        bool hasWhitespace = false;
        foreach (Rune rune in element.EnumerateRunes())
        {
            if (Rune.IsWhiteSpace(rune))
            {
                hasWhitespace = true;
                continue;
            }
            if (!IsInvisiblePersonaRune(rune))
                return false;
        }
        return hasWhitespace;
    }

    // Returns whether a rune should be removed from a presentation name
    private static bool IsInvisiblePersonaRune(Rune rune)
    {
        return Rune.GetUnicodeCategory(rune) is
            UnicodeCategory.Control or
            UnicodeCategory.Format or
            UnicodeCategory.LineSeparator or
            UnicodeCategory.ParagraphSeparator or
            UnicodeCategory.Surrogate or
            UnicodeCategory.OtherNotAssigned or
            UnicodeCategory.NonSpacingMark or
            UnicodeCategory.SpacingCombiningMark or
            UnicodeCategory.EnclosingMark;
    }

    // Overrides the C#-side scoreboard flair for a managed bot
}
