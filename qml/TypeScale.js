.pragma library

// One type scale for the reader page, in steps of the body text (Text size ×
// system scale): every size and the page's spacing are multiples of it, in
// every layout, so they grow and shrink together.
var title = 1.9          // chapter title
var heading = 1          // section headings, semi-bold
var note = 0.8           // margin notes
var number = 0.56        // verse numbers in the Verse layout
var mark = 0.55          // footnote marks
var headingAbove = 1     // space above a section heading
var headingBelow = 0.67  // and below it
var poetryIndent = 1.25  // per poetry level; wrapped lines hang one more
// The column's measure, in widths of the digit zero (CSS "ch").
var measure = 66

function size(body, step) { return Math.max(1, Math.round(body * step)) }
