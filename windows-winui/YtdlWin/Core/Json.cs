/* Tolerant readers over System.Text.Json, and the atomic write every file this
 * app owns goes through.
 *
 * WHY NOT A DESERIALISED MODEL. Every JSON file this app reads was written by
 * something else -- manifest.json by postprocess.ps1, info.json by yt-dlp,
 * ffprobe's output by ffmpeg -- and all three vary by version and by extractor.
 * A record with [JsonPropertyName] attributes is a schema, and the failure mode
 * of a schema against output like that is that ONE unexpected member fails the
 * whole document: a video with an odd info.json would vanish from the library
 * rather than showing up with a missing field.
 *
 * So the shape is read member by member, every accessor tolerates a missing
 * key, a null, and a value of the wrong type, and nothing here throws. That is
 * the same decision json-glib forced on the GTK app and that Foundation's
 * JSONSerialization got in the SwiftUI one, arrived at for the same reason
 * rather than copied.
 *
 * ffprobe in particular reports numbers as JSON strings more often than not
 * ("bit_rate": "128000") and omits a key entirely rather than sending null when
 * it does not know. Both are normal, neither is an error.
 *
 * JsonDocument is IDisposable and its JsonElements are only valid while it
 * lives, which is a trap that does not exist in the other two ports: a
 * JsonElement handed out of a `using` block reads freed memory. So every
 * document is cloned on the way out -- Clone() detaches the element from the
 * document's buffer -- and the document is disposed immediately. Cloning costs
 * a copy of the parsed tree, which for an info.json with a large comment tree
 * is real; it is paid once per video page, on a background thread, and it is
 * far cheaper than the alternative of keeping documents alive and having to
 * reason about who disposes them.
 */

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.Json;

namespace YtdlWin.Core;

public static class JsonFile
{
    private static readonly JsonDocumentOptions ReadOptions = new()
    {
        // yt-dlp does not emit comments or trailing commas, but a
        // hand-corrected manifest might, and refusing to read a file over a
        // trailing comma helps nobody.
        AllowTrailingCommas = true,
        CommentHandling = JsonCommentHandling.Skip,
        // A comment tree is nested shallowly; 128 is far past anything real and
        // still bounds a maliciously nested file.
        MaxDepth = 128,
    };

    /// Parse a file into an object element, or null. A malformed or absent file
    /// is an ordinary state the layout contract tells consumers to tolerate, so
    /// this reports nothing and lets the caller's fallback take over.
    public static JsonElement? Object(string path)
    {
        if (!Paths.IsRegularFile(path)) return null;
        try
        {
            var bytes = File.ReadAllBytes(Paths.Extended(path));
            return ObjectFrom(bytes);
        }
        catch (Exception) { return null; }
    }

    public static JsonElement? ObjectFrom(byte[] bytes)
    {
        if (bytes.Length == 0) return null;
        try
        {
            using var doc = JsonDocument.Parse(bytes, ReadOptions);
            if (doc.RootElement.ValueKind != JsonValueKind.Object) return null;
            return doc.RootElement.Clone();
        }
        catch (JsonException) { return null; }
        catch (Exception) { return null; }
    }

    public static JsonElement? ObjectFrom(string text)
        => ObjectFrom(Encoding.UTF8.GetBytes(text));

    /// Parse a file into an array element, or null.
    public static JsonElement? Array(string path)
    {
        if (!Paths.IsRegularFile(path)) return null;
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllBytes(Paths.Extended(path)), ReadOptions);
            if (doc.RootElement.ValueKind != JsonValueKind.Array) return null;
            return doc.RootElement.Clone();
        }
        catch (Exception) { return null; }
    }

    /* Written INDENTED and with the members in the order they were added.
     *
     * Indented because these files are meant to be opened and read: "delete
     * this folder and start over" is a complete answer partly because the
     * things in it can be understood. The SwiftUI app also sorts its keys so
     * that a diff of two saves is readable; System.Text.Json has no
     * sorted-keys option, so the writers below emit members in a fixed order
     * by hand, which achieves the same thing. */
    private static readonly JsonWriterOptions WriteOptions = new()
    {
        Indented = true,
        // The default encoder escapes anything non-ASCII, which turns a video
        // title into a wall of \uXXXX. These files are UTF-8 and nothing here
        // interpolates them into HTML.
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    public static byte[] Write(Action<Utf8JsonWriter> body)
    {
        using var stream = new MemoryStream();
        using (var writer = new Utf8JsonWriter(stream, WriteOptions))
        {
            body(writer);
        }
        return stream.ToArray();
    }
}

/* The tolerant accessors. Extension methods on JsonElement so that a chain
 * reads the way the Swift and C ports do. */
public static class JsonElementExtensions
{
    private static bool TryMember(this JsonElement self, string key, out JsonElement value)
    {
        value = default;
        if (self.ValueKind != JsonValueKind.Object) return false;
        return self.TryGetProperty(key, out value);
    }

    /// A non-empty string member, or null. An empty string is treated as absent
    /// because that is what the manifest writes for "we did not learn this".
    public static string? Str(this JsonElement self, string key)
    {
        if (!self.TryMember(key, out var v)) return null;
        if (v.ValueKind != JsonValueKind.String) return null;
        var s = v.GetString();
        return string.IsNullOrEmpty(s) ? null : s;
    }

    public static string? Str(this JsonElement? self, string key)
        => self.HasValue ? self.Value.Str(key) : null;

    /// An integer member. Accepts a JSON number or a numeric string, because
    /// ffprobe sends both for the same field depending on the stream. A JSON
    /// boolean is NOT a count and falls back.
    public static long Int(this JsonElement self, string key, long fallback = 0)
    {
        if (!self.TryMember(key, out var v)) return fallback;
        switch (v.ValueKind)
        {
            case JsonValueKind.Number:
                if (v.TryGetInt64(out var n)) return n;
                // A number too large for Int64, or one written as 1.0e3, still
                // has a double reading worth having.
                if (v.TryGetDouble(out var d) && d >= long.MinValue && d <= long.MaxValue)
                    return (long)d;
                return fallback;
            case JsonValueKind.String:
                var s = v.GetString();
                if (string.IsNullOrEmpty(s)) return fallback;
                if (long.TryParse(s, NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsed))
                    return parsed;
                if (double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out var pd) &&
                    pd >= long.MinValue && pd <= long.MaxValue)
                    return (long)pd;
                return fallback;
            default:
                return fallback;
        }
    }

    public static long Int(this JsonElement? self, string key, long fallback = 0)
        => self.HasValue ? self.Value.Int(key, fallback) : fallback;

    public static double Double(this JsonElement self, string key, double fallback = 0)
    {
        if (!self.TryMember(key, out var v)) return fallback;
        if (v.ValueKind == JsonValueKind.Number)
            return v.TryGetDouble(out var d) ? d : fallback;
        if (v.ValueKind == JsonValueKind.String)
        {
            var s = v.GetString();
            // InvariantCulture, always. ffprobe writes "23.976" whatever the
            // machine's locale is, and parsing that under a locale whose
            // decimal separator is a comma gives 23976 -- a frame rate three
            // orders of magnitude wrong, with nothing throwing.
            return double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out var pd)
                ? pd : fallback;
        }
        return fallback;
    }

    public static double Double(this JsonElement? self, string key, double fallback = 0)
        => self.HasValue ? self.Value.Double(key, fallback) : fallback;

    public static bool Bool(this JsonElement self, string key)
    {
        if (!self.TryMember(key, out var v)) return false;
        return v.ValueKind switch
        {
            JsonValueKind.True => true,
            JsonValueKind.False => false,
            JsonValueKind.Number => v.TryGetInt64(out var n) && n != 0,
            JsonValueKind.String => v.GetString() is "true" or "1" or "True",
            _ => false,
        };
    }

    public static bool Bool(this JsonElement? self, string key)
        => self.HasValue && self.Value.Bool(key);

    public static JsonElement? Obj(this JsonElement self, string key)
    {
        if (!self.TryMember(key, out var v)) return null;
        return v.ValueKind == JsonValueKind.Object ? v : null;
    }

    public static JsonElement? Obj(this JsonElement? self, string key)
        => self.HasValue ? self.Value.Obj(key) : null;

    /// The objects of an array member, skipping anything that is not one.
    public static List<JsonElement> Objects(this JsonElement self, string key)
    {
        var out_ = new List<JsonElement>();
        if (!self.TryMember(key, out var v) || v.ValueKind != JsonValueKind.Array) return out_;
        foreach (var item in v.EnumerateArray())
        {
            if (item.ValueKind == JsonValueKind.Object) out_.Add(item);
        }
        return out_;
    }

    public static List<JsonElement> Objects(this JsonElement? self, string key)
        => self.HasValue ? self.Value.Objects(key) : new List<JsonElement>();

    /// The non-empty strings of an array member.
    public static List<string> Strings(this JsonElement self, string key)
    {
        var out_ = new List<string>();
        if (!self.TryMember(key, out var v) || v.ValueKind != JsonValueKind.Array) return out_;
        foreach (var item in v.EnumerateArray())
        {
            if (item.ValueKind != JsonValueKind.String) continue;
            var s = item.GetString();
            if (!string.IsNullOrEmpty(s)) out_.Add(s);
        }
        return out_;
    }

    public static List<string> Strings(this JsonElement? self, string key)
        => self.HasValue ? self.Value.Strings(key) : new List<string>();
}

/* Temp file, then replace. Used by every file this app owns -- settings,
 * profiles, the queue and the history.
 *
 * The queue and the history are the same category of data as profiles.json:
 * losing a queue somebody built up, or the record of what ran overnight, is
 * losing real work. A truncated write from a crash or a full disk would take
 * all of it.
 *
 * File.Replace rather than File.Move, and this is the Windows-specific part.
 * Move REFUSES when the destination exists, which after the first save is
 * always -- Move(overwrite: true) exists but is not atomic in the sense that
 * matters, and Replace is the call that maps onto ReplaceFile, which swaps the
 * files in one operation and preserves the original's attributes. Replace also
 * fails outright when the destination does not exist yet, so the first save
 * takes the Move path. */
public static class AtomicFile
{
    public static bool Write(byte[]? data, string path)
    {
        if (data is null) return false;

        var dir = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(dir))
        {
            try { Directory.CreateDirectory(Paths.Extended(dir)); }
            catch (Exception) { return false; }
        }

        var tmp = path + ".tmp";
        try
        {
            File.WriteAllBytes(Paths.Extended(tmp), data);
        }
        catch (Exception) { return false; }

        try
        {
            if (File.Exists(Paths.Extended(path)))
            {
                /* The third argument is the backup file, and null means "do not
                 * keep one". ignoreMetadataErrors: true because the destination
                 * may live on a volume or a share whose ACL copy fails, and
                 * failing the whole save over an attribute copy would lose the
                 * data this function exists to protect. */
                File.Replace(Paths.Extended(tmp), Paths.Extended(path), null,
                             ignoreMetadataErrors: true);
            }
            else
            {
                File.Move(Paths.Extended(tmp), Paths.Extended(path));
            }
            return true;
        }
        catch (Exception)
        {
            try { File.Delete(Paths.Extended(tmp)); } catch (Exception) { /* best effort */ }
            return false;
        }
    }
}
