// Block comments with a leading star, Qt's //! lines, GNU's indented blocks
// and kernel-doc's name line are documentation too; so is a comment above
// an attribute whose message holds a `;`.

int anchor = 0;

/** Block comment documentation.
 *  Second line.
 */
int §(block)block();

//! Qt-style documentation.
int §(qt)qt();

/* Write formatted output to stdout.

   This function is a possible cancellation point and therefore not
   marked with __THROW.  */
int §(gnu)print(const char* format, ...);

/**
 * kernel_open - Opens a device.
 * @minor: The minor number of the device.
 *
 * Returns a descriptor, or a negative error code.
 */
int §(kernel_doc)kernel_open(int minor);

/// Opens the legacy device.
[[deprecated("use kernel_open; it checks the minor")]] int §(before_attribute)legacy_open(int minor);
