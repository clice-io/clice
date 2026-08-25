/// # Special Hover Targets
///
/// ## GTK-Doc and kernel-doc — recognize GObject Introspection annotations
///
/// - status: unsupported
/// - order: 9
/// - issues: clangd#2662
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
