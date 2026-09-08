/* The one value converter this app needs.
 *
 * Kept to one deliberately. x:Bind can call a method directly and a view model
 * can expose a Visibility property, and both are clearer than a converter --
 * VideoCardModel does exactly that for its three visibilities. A converter is
 * only reached for here because a ControlTemplate has no view model to put a
 * computed property on.
 */

using System;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Data;

namespace YtdlWin;

/// <summary>Collapses an element when its bound string is empty, so a row
/// without a description is not a row with a blank line under its label.</summary>
public sealed partial class EmptyStringToCollapsedConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, string language)
        => string.IsNullOrWhiteSpace(value as string) ? Visibility.Collapsed : Visibility.Visible;

    /* One-way only. A Visibility cannot say what string produced it, and a
     * converter that guessed would silently write a wrong value back the first
     * time somebody used it in a two-way binding. Throwing names the mistake at
     * the point it is made. */
    public object ConvertBack(object value, Type targetType, object parameter, string language)
        => throw new NotSupportedException(
            "EmptyStringToCollapsedConverter is one-way; a Visibility cannot be turned back " +
            "into the string that produced it.");
}
