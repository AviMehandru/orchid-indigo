/* One labelled option: a header, a one-line explanation, and a control.
 *
 * WHY THIS EXISTS AT ALL. The obvious answer on Windows is SettingsCard and
 * SettingsExpander, which look exactly like this and are what every modern
 * Windows settings page is built from. They are NOT part of the Windows App SDK
 * -- they ship in the WinUI Community Toolkit, which is a third-party NuGet
 * package. This repository's rule is that each app depends only on what its
 * platform already provides, and both the GTK and SwiftUI apps hold to it
 * (libadwaita's AdwPreferencesGroup and AppKit's grouped Form are in the box on
 * their platforms). Forty lines here is a smaller price than a dependency, and
 * it is the same trade the GTK app makes when it hand-rolls the five things
 * libadwaita has no widget for.
 *
 * The explanation under each label is the point of the shape. These were
 * tooltips once, and a tooltip nobody hovers over is not documentation -- the
 * reason to turn a pass off is the thing somebody needs to read at the moment
 * they are deciding.
 */

using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace YtdlWin.Views;

public sealed class OptionRow : ContentControl
{
    public static readonly DependencyProperty HeaderProperty =
        DependencyProperty.Register(nameof(Header), typeof(string), typeof(OptionRow),
            new PropertyMetadata(""));

    public static readonly DependencyProperty DescriptionProperty =
        DependencyProperty.Register(nameof(Description), typeof(string), typeof(OptionRow),
            new PropertyMetadata(""));

    public string Header
    {
        get => (string)GetValue(HeaderProperty);
        set => SetValue(HeaderProperty, value);
    }

    public string Description
    {
        get => (string)GetValue(DescriptionProperty);
        set => SetValue(DescriptionProperty, value);
    }

    public OptionRow()
    {
        /* The style is applied by key rather than by implicit type lookup.
         * DefaultStyleKey with no generic.xaml would leave the control with no
         * template at all -- it would lay out as a zero-height nothing, which is
         * the failure mode where the page renders and the options are simply
         * absent. */
        DefaultStyleKey = typeof(OptionRow);
        HorizontalContentAlignment = HorizontalAlignment.Right;
        VerticalContentAlignment = VerticalAlignment.Center;
        HorizontalAlignment = HorizontalAlignment.Stretch;
    }
}
