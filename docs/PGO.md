Using PGO
---------

* Configure with `-DGeneratePGO=On`, e.g.
  `cmake -DCMAKE_BUILD_TYPE=Release -DLTO=On -DGeneratePGO=On ../..`
* `make au`
* Run over a representative set of data and options, e.g.
  `src/au json2au /path/to/big/json file`
  `src/au grep <some params>`
  `...etc`
* Data will have been output into the build directory.
* Use the _same_ build directory and reconfigure cmake setting `GeneratePGO` off and `UsePGO` on:
  `cmake -DCGeneratePGO=Off -DCUsePGO=On`
* `make au`
* Profit!
