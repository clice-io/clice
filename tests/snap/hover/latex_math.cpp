/// # Special Hover Targets
///
/// ## LaTeX math in Doxygen — render `@f$ ... @f$` formulas
///
/// - status: unsupported
/// - order: 10
/// - issues: clangd#2669
///
/// Doxygen LaTeX math formulas are shown verbatim, not rendered as math.

/// The area of a circle is @f$ A = \pi r^2 @f$.
double circle_area(double r);
