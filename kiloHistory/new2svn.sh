kiloviewer stop
rm -f index.html
cp index.html.new index.html
rm -f kiloIndex.css
cp kiloIndex.css.new kiloIndex.css
rm -f server.py
cp server.py.new server.py
kiloviewer start
