package expo.modules.fieldstore

import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition
import java.io.File

// Kotlin is a pass-through here and nothing more. Every decision - what a record
// looks like, when to sync, which field an offset names - is made either in C++
// or in TypeScript, and this layer only carries bytes between them.
//
// Note what does NOT appear below: any function that returns a single field.
// The boundary is a batch by design; see fieldstore.cpp.
class FieldStoreModule : Module() {

    private external fun nativeOpen(dir: String): Boolean
    private external fun nativeClose()
    private external fun nativeCreateMany(packed: ByteArray): LongArray
    private external fun nativeUpdateU64(ids: LongArray, offset: Int, values: LongArray)
    private external fun nativeReadMany(ids: LongArray): ByteArray
    private external fun nativeReadAll(limit: Int): ByteArray
    private external fun nativeSync()
    private external fun nativeCheckpoint(): Boolean
    private external fun nativeStats(): LongArray

    companion object {
        init { System.loadLibrary("fieldstore") }
    }

    override fun definition() = ModuleDefinition {
        Name("FieldStoreModule")

        // The engine takes its directory at run time, which is exactly what a
        // sandboxed process needs - the app's files dir is not knowable when
        // the C++ is compiled.
        Function("open") {
            val dir = File(appContext.reactContext!!.filesDir, "fieldstore")
            dir.mkdirs()
            nativeOpen(dir.absolutePath)
        }

        Function("close") { nativeClose() }
        Function("createMany") { packed: ByteArray -> nativeCreateMany(packed) }
        Function("updateU64") { ids: LongArray, offset: Int, values: LongArray ->
            nativeUpdateU64(ids, offset, values)
        }
        Function("readMany") { ids: LongArray -> nativeReadMany(ids) }
        Function("readAll") { limit: Int -> nativeReadAll(limit) }
        Function("sync") { nativeSync() }
        Function("checkpoint") { nativeCheckpoint() }
        Function("stats") { nativeStats() }
    }
}
