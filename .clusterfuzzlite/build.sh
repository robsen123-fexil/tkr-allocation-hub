#!/bin/bash -eu

# JVM + native JNI build for tkr-allocation-hub (Jazzer / ClusterFuzzLite)

mkdir -p "$OUT"/native "$OUT"/libs build/classes build/jar-native/native

# Jazzer artifacts (ClusterFuzzLite / OSS-Fuzz base-builder-jvm)
if [ ! -f "$OUT/jazzer_driver" ]; then
  cp "$(which jazzer_driver)" "$OUT/jazzer_driver"
fi
if [ ! -f "$OUT/jazzer_agent_deploy.jar" ]; then
  cp "$(which jazzer_agent_deploy.jar)" "$OUT/jazzer_agent_deploy.jar"
fi
if [ ! -f "$OUT/jazzer_driver_with_sanitizer" ]; then
  cat > "$OUT/jazzer_driver_with_sanitizer" << 'EOF'
#!/bin/bash
this_dir=$(dirname "$0")
"$this_dir/jazzer_driver" --asan "$@"
EOF
  chmod +x "$OUT/jazzer_driver_with_sanitizer"
fi

# Compile product sources
if [ -f "./gradlew" ]; then
  ./gradlew compileJava jar -x test --no-daemon
  cp build/libs/tkr-allocation-hub-*.jar "$OUT"/tkr-allocation-hub.jar 2>/dev/null || \
    cp build/libs/*.jar "$OUT"/tkr-allocation-hub.jar
else
  find src/main/java -name '*.java' > sources.txt
  javac -encoding UTF-8 -d build/classes @sources.txt
  jar cf "$OUT"/tkr-allocation-hub.jar -C build/classes .
fi

# Native JNI library: fuzzer-no-link for coverage + address for ASAN UAF sites
JVM_INCLUDES="-I${JAVA_HOME}/include -I${JAVA_HOME}/include/linux"
NATIVE_SAN="-fsanitize=fuzzer-no-link,address -fno-omit-frame-pointer"
$CXX $CXXFLAGS $NATIVE_SAN $JVM_INCLUDES -fPIC -shared \
  native/tkr_native.cpp \
  -o "$OUT"/native/libtkr_native.so

# Ship native library beside fuzzer, inside jar, and on LD_LIBRARY_PATH
cp "$OUT"/native/libtkr_native.so "$OUT"/libtkr_native.so
cp "$OUT"/native/libtkr_native.so build/jar-native/native/libtkr_native.so
jar uf "$OUT"/tkr-allocation-hub.jar -C build/jar-native native

PROJECT_JARS="tkr-allocation-hub.jar"
BUILD_CLASSPATH=$(echo $PROJECT_JARS | xargs printf -- "$OUT/%s:"):$JAZZER_API_PATH
RUNTIME_CLASSPATH=$(echo $PROJECT_JARS | xargs printf -- "\$this_dir/%s:"):\$this_dir:\$this_dir/native

# Compile fuzz harnesses
for fuzzer in $(find src/fuzz/java -name '*Fuzzer.java'); do
  fuzzer_basename=$(basename -s .java "$fuzzer")
  javac -encoding UTF-8 -cp "$BUILD_CLASSPATH" -d "$OUT" "$fuzzer"

  driver=jazzer_driver_with_sanitizer
  echo "#!/bin/bash
# LLVMFuzzerTestOneInput for fuzzer detection.
this_dir=\$(dirname \"\$0\")
cd \"\$this_dir\"
export TKR_NATIVE_LIB=\"\$this_dir/native/libtkr_native.so\"
LD_LIBRARY_PATH=\"\$this_dir/native:\$this_dir:\$JVM_LD_LIBRARY_PATH\" \
ASAN_OPTIONS=\$ASAN_OPTIONS:symbolize=1:detect_leaks=0:abort_on_error=1:handle_abort=1 \
\$this_dir/$driver --agent_path=\$this_dir/jazzer_agent_deploy.jar \
--cp=$RUNTIME_CLASSPATH \
--target_class=com.tkr.fuzz.$fuzzer_basename \
--jvm_args=\"-Xmx2048m:-Xss1024k:-Djava.awt.headless=true:-Duser.dir=\$this_dir:-Djava.library.path=\$this_dir/native:\$this_dir:-Dtkr.native.path=\$this_dir/native:-Dtkr.native.lib=\$this_dir/native/libtkr_native.so\" \
\$@" > "$OUT/$fuzzer_basename"
  chmod +x "$OUT/$fuzzer_basename"
done

echo "JVM build complete -> $OUT"
