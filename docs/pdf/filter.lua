-- Pandoc filter for the PDF manual.
-- Links to other pages of the repository (foo.md, ../examples/x.py, docs/..) cannot work inside one PDF: keep the text, drop
-- the link. Links inside a page (#anchor) and to the web (https://...) stay.
function Link(el)
  local t = el.target
  if t:match("^#") or t:match("^https?://") or t:match("^mailto:") then
    return nil
  end
  return el.content
end

-- an image whose file is missing must not stop the build
function Image(el)
  return nil
end

-- a fenced block without a language still gets wrapped lines (see header.tex)
function CodeBlock(el)
  return nil
end
