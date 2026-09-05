/// # LaTeX math in Doxygen
///
/// - status: unsupported
/// - issues: clangd#2669
///
/// Render `@f$ ... @f$` formulas
///
/// Doxygen LaTeX math formulas are shown verbatim, not rendered as math.

/// The area of a circle is @f$ A = \pi r^2 @f$.
double circle_area(double r);
