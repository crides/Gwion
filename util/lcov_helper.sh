#!/bin/bash

#make all

#source util/test.sh

#do_test severity=11 ASYNC=0 tests/sh/ examples tests/error tests/tree tests/bug tests/ugen_coverage | consummer

lcov --capture --directory core --directory eval/absyn.c --directory lang --directory ugen --output-file gwion.info

genhtml -o lcov gwion.info

cat << EOF > lcov/helper_gcov.js
var i = 1;
var cusid_ele = document.getElementsByClassName('LineNoCov');
function keyDownTextField(e) {
var keyCode = e.keyCode;
  if(keyCode==78) {
    var item = cusid_ele[i];
    cusid_ele[i].scrollIntoView();
    i++;
  } else if(keyCode==66){
    i--;
    var item = cusid_ele[i];
    cusid_ele[i].scrollIntoView();
  }
  if(i < 1) i = 1;
  var size = cusid_ele.length - 1;
  if(i > size) i = size;
}
document.addEventListener("keydown", keyDownTextField, false);
EOF

cat << EOF > lcov/helper_func.js
var i = 1;
var cusid_ele = document.getElementsByClassName('coverFnLo');
function keyDownTextField(e) {
var keyCode = e.keyCode;
  if(keyCode==78) {
    var item = cusid_ele[i];
    cusid_ele[i].scrollIntoView();
    i++;
  } else if(keyCode==66){
    i--;
    var item = cusid_ele[i];
    cusid_ele[i].scrollIntoView();
  }
  if(i < 1) i = 1;
  var size = cusid_ele.length - 1;
  if(i > size) i = size;
}
document.addEventListener("keydown", keyDownTextField, false);
EOF

for file in lcov/*/*.*.gcov.html
do sed -i "s/<body>/<body><script src=\"..\/helper_gcov.js\"><\/script>/" "$file"
done

for file in lcov/*/*.*.func*.html
do sed -i "s/<body>/<body><script src=\"..\/helper_func.js\"><\/script>/" "$file"
done

