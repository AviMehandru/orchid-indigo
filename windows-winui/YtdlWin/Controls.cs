/* The handful of pieces every pane uses.
 *
 * Built in code rather than as templated controls with their own XAML. Three
 * small pieces do not earn a ResourceDictionary and a ControlTemplate each, and
 * a factory method is far easier to read against the GTK and SwiftUI versions of
 * the same thing -- which matters more than usual on a fifth implementation
 * nobody can diff by running.
 *
 * No colour is named here. Everything comes from a ThemeResource, so all of it
 * follows the system light/dark setting and the user's accent colour. That
 * following-the-system property is the whole point of the GTK app's stylesheet,
 * and it is the reason a hand-picked palette would be a downgrade rather than a
 * consistency win.
 */

using System;
using System.Collections.Generic;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using YtdlWin.Core;

namespace YtdlWin;

public enum PillVariant { Neutral, Ok, Warn, Error, Accent }

public static class Controls
{
    /* A coloured pill rather than a coloured word. On a table of seven rows the
     * SHAPE is what you scan -- "missing" in red text beside "required" in grey
     * text reads as one run-on phrase. */
    public static Border Pill(string text, PillVariant variant = PillVariant.Neutral)
    {
        var brushKey = variant switch
        {
            PillVariant.Ok => "SystemFillColorSuccessBrush",
            PillVariant.Warn => "SystemFillColorCautionBrush",
            PillVariant.Error => "SystemFillColorCriticalBrush",
            PillVariant.Accent => "AccentTextFillColorPrimaryBrush",
            _ => "TextFillColorSecondaryBrush",
        };
        var backgroundKey = variant switch
        {
            PillVariant.Ok => "SystemFillColorSuccessBackgroundBrush",
            PillVariant.Warn => "SystemFillColorCautionBackgroundBrush",
            PillVariant.Error => "SystemFillColorCriticalBackgroundBrush",
            _ => "SubtleFillColorSecondaryBrush",
        };

        return new Border
        {
            Background = Resource<Brush>(backgroundKey),
            CornerRadius = new CornerRadius(9),
            Padding = new Thickness(8, 2, 8, 2),
            VerticalAlignment = VerticalAlignment.Center,
            Child = new TextBlock
            {
                Text = text,
                FontSize = 11,
                FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                Foreground = Resource<Brush>(brushKey),
            },
        };
    }

    /// One fact per row: the name on the left, the value on the right.
    /// Selectable, because most of these are paths and ids somebody wants to
    /// paste somewhere.
    public static Grid KeyValueRow(string key, string? value, bool monospaced = false)
    {
        var grid = new Grid { ColumnSpacing = 12, Margin = new Thickness(0, 0, 0, 4) };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(160) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

        var name = new TextBlock
        {
            Text = key,
            Foreground = Resource<Brush>("TextFillColorSecondaryBrush"),
            TextWrapping = TextWrapping.Wrap,
        };
        Grid.SetColumn(name, 0);
        grid.Children.Add(name);

        var body = new TextBlock
        {
            Text = string.IsNullOrEmpty(value) ? "—" : value,
            IsTextSelectionEnabled = true,
            /* Wrapping, not truncating. A TextBlock defaults to NoWrap, and a
             * non-wrapping TextBlock reports its minimum width as its natural
             * width -- which is how one long path in a row like this ends up
             * setting the whole window's minimum width. The GTK app hit exactly
             * that and traced it to a single unwrapped label. */
            TextWrapping = TextWrapping.Wrap,
        };
        if (monospaced)
        {
            body.FontFamily = new FontFamily("Consolas, Cascadia Mono, Courier New");
        }
        Grid.SetColumn(body, 1);
        grid.Children.Add(body);

        return grid;
    }

    /// A titled block of rows, which is what the Health and Detail panes are
    /// made of.
    /* `trailing` is a FrameworkElement, not a UIElement, and that is a WinUI
     * signature rather than a preference: Grid.SetColumn takes a
     * FrameworkElement here, where the WPF overload of the same name takes a
     * UIElement. Every other Grid.SetColumn call site in this app passes a
     * concrete Border, Button, TextBlock or StackPanel and so never noticed;
     * this one, declared as the base type, is the only place it shows up. */
    public static StackPanel SectionBox(string title, FrameworkElement? trailing,
                                        params UIElement[] rows)
    {
        var header = new Grid();
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        var titleBlock = new TextBlock
        {
            Text = title,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            VerticalAlignment = VerticalAlignment.Center,
        };
        Grid.SetColumn(titleBlock, 0);
        header.Children.Add(titleBlock);

        if (trailing is not null)
        {
            Grid.SetColumn(trailing, 1);
            header.Children.Add(trailing);
        }

        var body = new StackPanel { Spacing = 2 };
        foreach (var row in rows) body.Children.Add(row);

        var card = new Border
        {
            Background = Resource<Brush>("CardBackgroundFillColorDefaultBrush"),
            BorderBrush = Resource<Brush>("CardStrokeColorDefaultBrush"),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(8),
            Padding = new Thickness(12),
            Child = body,
        };

        return new StackPanel
        {
            Spacing = 8,
            Margin = new Thickness(0, 0, 0, 20),
            Children = { header, card },
        };
    }

    /// Look a theme brush up out of the application's resources, falling back
    /// to a transparent brush rather than throwing. A missing key is a typo,
    /// and a typo should not take a whole page down at runtime.
    public static T? Resource<T>(string key) where T : class
    {
        try
        {
            if (Application.Current.Resources.TryGetValue(key, out var value)) return value as T;
        }
        catch (Exception) { /* called before the app's resources exist */ }
        return null;
    }
}

/* Thumbnails: decoded ONCE and reused as the grid scrolls, and decoded SCALED.
 *
 * A 1920x1080 thumbnail decoded whole costs about eight megabytes of bitmap per
 * card, and an archive has thousands of cards. DecodePixelWidth is what does the
 * scaling DURING the decode rather than after it -- and it only works if it is
 * set BEFORE the source, which is the whole trap in this API: assign UriSource
 * first and the full frame is already decoded by the time the hint is read, so
 * the property appears to work (the image is the right size) while buying
 * nothing at all.
 *
 * The cache is bounded and holds BitmapImages, not files. Clearing it on rescan
 * is what stops a renamed or replaced thumbnail showing the old picture.
 */
public static class ThumbnailCache
{
    private const int MaxEntries = 600;
    private const int DecodeWidth = 480;

    private static readonly object Lock = new();
    private static readonly Dictionary<string, BitmapImage> Cache = new(StringComparer.OrdinalIgnoreCase);
    private static readonly LinkedList<string> Order = new();

    /// Must be called on the UI thread: a BitmapImage has thread affinity, and
    /// constructing one off the dispatcher throws.
    public static BitmapImage? Load(string? path)
    {
        if (string.IsNullOrEmpty(path)) return null;

        lock (Lock)
        {
            if (Cache.TryGetValue(path, out var hit))
            {
                Order.Remove(path);
                Order.AddLast(path);
                return hit;
            }
        }

        BitmapImage image;
        try
        {
            image = new BitmapImage
            {
                /* Set BEFORE UriSource. See the header -- this ordering is the
                 * entire optimisation. */
                DecodePixelWidth = DecodeWidth,
                DecodePixelType = DecodePixelType.Logical,
            };
            image.UriSource = new Uri(path);
        }
        catch (Exception)
        {
            /* A path that is not a valid URI, or an image format Windows has no
             * decoder for -- .avif on an older build is the realistic case, and
             * the pipeline does write .avif thumbnails. The card shows its
             * placeholder, which is the right outcome. */
            return null;
        }

        lock (Lock)
        {
            Cache[path] = image;
            Order.AddLast(path);
            while (Order.Count > MaxEntries && Order.First is { } oldest)
            {
                Cache.Remove(oldest.Value);
                Order.RemoveFirst();
            }
        }
        return image;
    }

    public static void Clear()
    {
        lock (Lock)
        {
            Cache.Clear();
            Order.Clear();
        }
    }
}
