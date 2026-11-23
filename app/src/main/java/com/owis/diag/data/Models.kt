package com.owis.diag.data

import org.json.JSONArray

data class PackInfo(val id: String, val title: String, val path: String)
data class Vehicle(val vin: String, val packId: String, val title: String)
data class Fault(val code: String, val text: String, val status: String, val env: String)
data class ModuleScan(val name: String, val id: Int, val proto: String, val ident: String, val faults: List<Fault>)
data class ModuleRef(val index: Int, val key: String, val name: String)
data class Fn(val index: Int, val cat: String, val name: String)
data class Job(val id: String, val title: String, val note: String)

object Parse {
    fun packs(json: String): List<PackInfo> {
        val a = JSONArray(json); return (0 until a.length()).map { val o = a.getJSONObject(it)
            PackInfo(o.getString("id"), o.getString("title"), o.getString("path")) }
    }
    fun scan(json: String): List<ModuleScan> {
        val a = JSONArray(json); return (0 until a.length()).map { i ->
            val o = a.getJSONObject(i); val fa = o.getJSONArray("faults")
            ModuleScan(o.getString("name"), o.getInt("id"), o.getString("proto"), o.getString("ident"),
                (0 until fa.length()).map { j -> val f = fa.getJSONObject(j)
                    Fault(f.getString("code"), f.getString("text"), f.getString("status"), f.getString("env")) })
        }
    }
    fun modules(json: String): List<ModuleRef> {
        val a = JSONArray(json); return (0 until a.length()).map { val o = a.getJSONObject(it)
            ModuleRef(o.getInt("i"), o.getString("key"), o.getString("name")) }
    }
    fun functions(json: String): List<Fn> {
        val a = JSONArray(json); return (0 until a.length()).map { val o = a.getJSONObject(it)
            Fn(o.getInt("i"), o.getString("cat"), o.getString("name")) }
    }
    fun jobs(json: String): List<Job> {
        val a = JSONArray(json); return (0 until a.length()).map { val o = a.getJSONObject(it)
            Job(o.getString("id"), o.getString("title"), o.getString("note")) }
    }
    fun vehicle(json: String): Vehicle? {
        val o = org.json.JSONObject(json); if (!o.has("packId")) return null
        return Vehicle(o.optString("vin"), o.getString("packId"), o.optString("title"))
    }
}
