package com.autod.controller;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.graphics.drawable.Drawable;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.ImageView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * 应用列表适配器。
 *
 * <p>这里体现了 daemon 与上位应用的分工：daemon 只返回包名和廉价元数据
 * （{@code pm list packages} 给不出标签，逐个 dumpsys 又太慢），
 * 标签和图标由上位应用通过 PackageManager 解析 —— 一个调用就有，
 * 而且是本地化过的。
 *
 * <p>图标做了缓存：一个 100+ 应用的列表如果每次都重新取 Drawable，
 * 滚动会明显卡顿。
 */
public class AppListAdapter extends BaseAdapter {

    public static final class Item {
        public String pkg;
        public long versionCode;
        public boolean system;
        public String apkPath;

        // 由 PackageManager 解析出来
        public CharSequence label;
        public Drawable icon;
        public boolean installedLocally = true;

        @Override
        public String toString() { return pkg; }
    }

    private final Context ctx;
    private final PackageManager pm;
    private final List<Item> items = new ArrayList<>();
    private final Map<String, Drawable> iconCache = new HashMap<>();
    private final LayoutInflater inflater;

    public AppListAdapter(Context ctx) {
        this.ctx = ctx;
        this.pm = ctx.getPackageManager();
        this.inflater = LayoutInflater.from(ctx);
    }

    public void setItems(List<Item> newItems) {
        items.clear();
        if (newItems != null) items.addAll(newItems);
        notifyDataSetChanged();
    }

    public Item getItemAt(int position) {
        return items.get(position);
    }

    @Override public int getCount() { return items.size(); }
    @Override public Object getItem(int position) { return items.get(position); }
    @Override public long getItemId(int position) { return position; }

    @Override
    public View getView(int position, View convertView, ViewGroup parent) {
        View v = convertView;
        if (v == null) {
            v = inflater.inflate(android.R.layout.simple_list_item_2, parent, false);
        }
        TextView title = v.findViewById(android.R.id.text1);
        TextView sub   = v.findViewById(android.R.id.text2);
        ImageView icon = null;   // simple_list_item_2 没有图标位，复用 text1 的复合图标

        Item it = items.get(position);

        if (it.label == null) resolve(it);

        title.setText(it.label != null ? it.label : it.pkg);
        if (icon != null) icon.setImageDrawable(it.icon);

        StringBuilder sb = new StringBuilder(it.pkg);
        if (it.versionCode > 0) sb.append("  v").append(it.versionCode);
        if (it.system) sb.append("  · 系统");
        sub.setText(sb);

        return v;
    }

    /** 解析标签与图标；解析不到时退化为包名，而不是显示空白 */
    private void resolve(Item it) {
        try {
            ApplicationInfo ai = pm.getApplicationInfo(it.pkg, 0);
            it.label = pm.getApplicationLabel(ai);
            Drawable cached = iconCache.get(it.pkg);
            if (cached == null) {
                cached = pm.getApplicationIcon(ai);
                iconCache.put(it.pkg, cached);
            }
            it.icon = cached;
            it.installedLocally = true;
        } catch (PackageManager.NameNotFoundException e) {
            // daemon 列出的包，上位应用看不到 —— 说明两者的可见范围不一致
            // （比如 daemon 以 root 跑，能看到所有用户的应用）。
            // 显示包名总比显示空白强。
            it.label = it.pkg + "（未安装于当前用户）";
            it.installedLocally = false;
        }
    }
}
