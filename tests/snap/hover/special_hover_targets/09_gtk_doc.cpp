/// # GTK-Doc and kernel-doc
///
/// - status: unsupported
/// - issues: clangd#2662
///
/// Recognize GObject Introspection annotations
///
/// GTK-Doc / kernel-doc comment syntax and GObject Introspection
/// annotations are not parsed into the hover card.

/**
 * gtk_widget_show:
 * @widget: (transfer none): a #GtkWidget
 *
 * Flags a widget to be displayed.
 */
void gtk_widget_show(GtkWidget *widget);
