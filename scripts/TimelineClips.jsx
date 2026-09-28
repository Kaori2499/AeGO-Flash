/* AeGO Flash: editable AE timeline clips. ExtendScript / expressions use ES3.
 * Native code loads this file, then calls L2DAE_addClips with one motion and/or expression.
 * Clip metadata lives in a layer marker so rename, duplicate, split, trim,
 * reorder, and native time stretch preserve the association and source clock.
 */
(function (global) {
    var MATCH = "L2DAE Native Renderer";
    var PREFIX = "L2DCLIP|";
    var HEADER = "// L2DAE_TIMELINE_V2 owner=";
    var CONTROL_NAMES = ["Motion A index", "Motion B index", "Motion A time (seconds)",
                         "Motion B time (seconds)", "Transition A to B (%)"];
    var MOTION_CHANNEL = {
        kind: "motion", binding: "Timeline binding", prefix: PREFIX, tag: "L2DCLIP",
        controls: CONTROL_NAMES,
        legacy: ["Motion A slot", "Motion B slot"],
        headerPattern: /^\/\/ L2DAE_TIMELINE_V[12] owner=(\d+)(?:\r\n|\n|\r)/
    };
    var EXPRESSION_CHANNEL = {
        kind: "expression", binding: "Expression binding", prefix: "L2DEXPR|", tag: "L2DEXPR",
        controls: ["Expression A index", "Expression B index", "Expression A weight (%)", "Expression B weight (%)"],
        legacy: ["Expression A slot", "Expression B slot"],
        headerPattern: /^\/\/ L2DAE_EXPRESSION_V[12] owner=(\d+)(?:\r\n|\n|\r)/
    };
    // AE builds each parameter matchName from the effect matchName and its
    // persistent PF disk ID. Display names can change with localization, while
    // these IDs retain the same saved stream, keys and expressions across versions.
    var PARAMETER_IDS = {
        "Loop motion": 103, "Motion A slot": 109, "Motion B slot": 110,
        "Keyframe motion time": 111, "Motion A time (seconds)": 112,
        "Motion B time (seconds)": 113, "Transition A to B (%)": 114,
        "Timeline binding": 115, "Expression A slot": 118, "Expression B slot": 119,
        "Expression A weight (%)": 120, "Expression B weight (%)": 121,
        "Expression binding": 122, "Motion A index": 134, "Motion B index": 135,
        "Expression A index": 136, "Expression B index": 137
    };
    var CHINESE = false;
    try { CHINESE = /^zh[-_]cn$/i.test(String(app.isoLanguage)); } catch (ignored) { CHINESE = false; }
    function text(english, chinese) { return CHINESE ? chinese : english; }

    function fail(message) { throw new Error("AeGO Flash: " + message); }
    function finite(value) { return typeof value === "number" && isFinite(value); }
    function integer(value) { return finite(value) && Math.floor(value) === value; }
    function findParameter(group, name) {
        var id = PARAMETER_IDS[name];
        if (!id) fail(text("Unrecognized effect parameter. Check that the plug-in and script versions match.", "无法识别效果参数，请检查插件与脚本版本。"));
        var match = MATCH + "-" + ("0000" + id).slice(-4);
        function find(parent) {
            var direct = parent.property(match);
            if (direct && direct.matchName === match) return direct;
            // Topic groups may be represented as nested properties by a host.
            // Search only stable match names; never accept a renamed UI label.
            for (var i = 1; i <= parent.numProperties; ++i) {
                var child = parent.property(i);
                if (child && typeof child.numProperties === "number" && typeof child.property === "function") {
                    var found = find(child);
                    if (found) return found;
                }
            }
            return null;
        }
        return find(group);
    }
    function property(group, name) {
        var result = findParameter(group, name);
        if (!result) fail(text("Missing AeGO Flash parameter (id " + PARAMETER_IDS[name] + "). Reinstall the complete plug-in folder.", "缺少必要的 AeGO Flash 效果参数（编号 " + PARAMETER_IDS[name] + "），请完整更新插件文件夹。"));
        return result;
    }
    function ownerFromExpression(value, channel) {
        var match = channel.headerPattern.exec(value || "");
        return match ? Number(match[1]) : 0;
    }
    function markerData(value, owner, channel) {
        var p = value.getParameters(), slot, duration, fade, curve = 0;
        if (p.l2daeKind === channel.kind && Number(p.l2daeOwner) === owner) {
            slot = Number(p.l2daeSlot); duration = Number(p.l2daeDuration); fade = Number(p.l2daeFade);
            if (typeof p.l2daeCurve !== "undefined") curve = Number(p.l2daeCurve);
        } else {
            var parts = String(value.comment).split("|");
            if (parts.length !== 4 || parts[0] !== channel.tag || Number(parts[1]) !== owner) return null;
            slot = Number(parts[2]); duration = Number(parts[3]); fade = 0.2;
        }
        if (!integer(slot) || slot < 1 || slot > 16777216 || !finite(duration) || duration <= 0 ||
            !finite(fade) || fade < 0 || !integer(curve) || curve < 0 || curve > 5) return null;
        return { slot: slot, duration: duration, fade: fade, curve: curve };
    }
    function clipName(label) {
        return String(label).replace(/^.*[\\\/]/, "").replace(/^L2D(?: 表情)? · /, "")
            .replace(/\.(?:motion3|exp3)\.json$/i, "").replace(/[\r\n\t]/g, " ").substr(0, 200);
    }
    function setMetadata(value, channel, owner, slot, duration, fade, label, curve) {
        var p = value.getParameters();
        p.l2daeKind = channel.kind; p.l2daeOwner = String(owner); p.l2daeSlot = String(slot);
        p.l2daeDuration = String(duration); p.l2daeFade = String(fade);
        p.l2daeCurve = String(curve);
        value.setParameters(p); value.comment = clipName(label);
    }
    function defaultNullName(name) {
        // Only recognizable host-generated null names are migrated. A renamed
        // source belongs to the user, even when all its layers are our clips.
        return /^(?:Null|Nullobjekt|Null-Objekt|Nul|Nulo|Nullo|\u7a7a|\u30cc\u30eb|\ub110|\u041d\u0443\u043b\u044c)\s*\d+$/i.test(String(name));
    }
    function renameClipSources(candidates, existingItems, renamed) {
        var seen = {}, i, j, k, n;
        for (i = 0; i < candidates.length; ++i) {
            var candidate = candidates[i], source = candidate.layer.source;
            if (!source || typeof source.id !== "number" || !candidate.layer.nullLayer) continue;
            var key = "item" + source.id;
            if (seen[key]) continue;
            seen[key] = true;
            // Already repaired sources are the common case on later imports;
            // avoid scanning every clip/usage again for each named source.
            if (source.name === candidate.name ||
                (existingItems[key] && (!candidate.migrate || !defaultNullName(source.name)))) continue;
            var owned = [], safe = true;
            for (j = 0; j < candidates.length; ++j) {
                var other = candidates[j], otherSource = other.layer.source;
                if (!otherSource || otherSource.id !== source.id) continue;
                if (!other.layer.nullLayer || other.name !== candidate.name ||
                    (existingItems[key] && !other.migrate)) safe = false;
                owned.push(other);
            }
            if (!safe) continue;
            // usedIn lists compositions, not layers. Inspect every usage so a
            // shared null in another model/comp or an unrelated layer is untouched.
            var comps = source.usedIn;
            for (j = 0; j < comps.length && safe; ++j) {
                var comp = comps[j];
                for (k = 1; k <= comp.numLayers; ++k) {
                    var layer = comp.layer(k), usedSource = layer.source;
                    if (!usedSource || usedSource.id !== source.id) continue;
                    var found = false;
                    for (n = 0; n < owned.length; ++n) {
                        if (owned[n].comp.id === comp.id && owned[n].layer.index === layer.index) { found = true; break; }
                    }
                    if (!found) { safe = false; break; }
                }
            }
            if (safe) {
                renamed.push({ source: source, name: source.name });
                source.name = candidate.name;
            }
        }
    }
    function findClips(comp, owner, channel) {
        var result = [], i, k, layer, markers, value, data;
        if (!owner) return result;
        for (i = 1; i <= comp.numLayers; ++i) {
            layer = comp.layer(i);
            markers = layer.property("ADBE Marker");
            if (!markers) continue;
            for (k = 1; k <= markers.numKeys; ++k) {
                value = markers.keyValue(k);
                data = markerData(value, owner, channel);
                if (data) {
                    result.push({ layer: layer, key: k, comment: value.comment,
                                  slot: data.slot, duration: data.duration, fade: data.fade, curve: data.curve,
                                  begin: Math.min(layer.inPoint, layer.outPoint),
                                  end: Math.max(layer.inPoint, layer.outPoint) });
                    break;
                }
            }
        }
        return result;
    }
    function ownedByAnotherEffect(comp, target, owner, channel) {
        if (!owner) return false;
        for (var i = 1; i <= comp.numLayers; ++i) {
            var effects = comp.layer(i).property("ADBE Effect Parade");
            if (!effects) continue;
            for (var j = 1; j <= effects.numProperties; ++j) {
                var effect = effects.property(j);
                if (effect.matchName !== MATCH || (i === target.layer.index && j === target.effect.propertyIndex)) continue;
                var binding = findParameter(effect, channel.binding);
                var control = findParameter(effect, channel.controls[0]);
                var legacy = findParameter(effect, channel.legacy[0]);
                if ((binding && Number(binding.value) === owner) ||
                    (control && ownerFromExpression(control.expression, channel) === owner) ||
                    (legacy && ownerFromExpression(legacy.expression, channel) === owner)) return true;
            }
        }
        return false;
    }
    function expressionScan(channel) {
        // Marker parameters keep the visible comment free for the imported name.
        // Legacy markers remain readable until the next import migrates the group.
        return "for(var i=1;i<=thisComp.numLayers;i++){\n" +
            " var l=thisComp.layer(i), m=l.marker; if(!l.enabled) continue;\n" +
            " for(var k=1;k<=m.numKeys;k++){\n" +
            "  var key=m.key(k), p=key.parameters, s,d,f,q=0,valid=false;\n" +
            "  if(p && p.l2daeKind=='" + channel.kind + "' && Number(p.l2daeOwner)==owner){s=Number(p.l2daeSlot);d=Number(p.l2daeDuration);f=Number(p.l2daeFade);if(typeof p.l2daeCurve!='undefined')q=Number(p.l2daeCurve);valid=true;}\n" +
            "  else {var old=String(key.comment).split('|'); if(old.length==4 && old[0]=='" + channel.tag + "' && Number(old[1])==owner){s=Number(old[2]);d=Number(old[3]);f=0.2;valid=true;}}\n" +
            "  if(valid && s>=1 && s<=16777216 && Math.floor(s)==s && isFinite(d) && d>0 && isFinite(f) && f>=0 && isFinite(q) && Math.floor(q)===q && q>=0 && q<=5){\n" +
            "   var a=Math.min(l.inPoint,l.outPoint), b=Math.max(l.inPoint,l.outPoint);\n" +
            "   if(b>a) clips.push({layer:l,slot:s,duration:d,fade:f,curve:q,begin:a,end:b,index:i}); break;\n" +
            "  }\n" +
            " }\n" +
            "}\n" +
            "clips.sort(function(a,b){return a.begin-b.begin || b.index-a.index;});\n" +
            "function edge(delta,fade){return fade>0?Math.max(0,Math.min(1,delta/fade)):1;}\n";
    }
    function expression(owner, output) {
        // Literal owner retains an established group after a failed later import.
        return HEADER + owner + "\n" +
            "(function(){\n" +
            "var owner=" + owner + ", output=" + output + ", clips=[], active=[], t=time;\n" +
            expressionScan(MOTION_CHANNEL) +
            "function clock(c,u){return Math.max(0,Math.min(c.duration,c.layer.sourceTime(u)));}\n" +
            // Retain curve 1's bounded rebound for previously saved clips.
            // The incoming clip owns the curve; reversed playback changes only
            // the source clock, while a nested clip's exit reverses this envelope.
            "function spring(u){if(u<=0)return 0;if(u>=1)return 1;var r=1-u,c=Math.cos(2*Math.PI*u);return 1-r*r*r*c*c;}\n" +
            // Curve 2 eases in, overshoots B by 4.96% at u=0.8, then settles.
            // Both endpoint velocities are zero. Do not clamp the shared weight:
            // the native renderer must receive the small extrapolation above 1.
            "function overshoot(u){if(u<=0)return 0;if(u>=1)return 1;var r=1-u;return 3*u*u-2*u*u*u+7.5*u*u*u*r*r;}\n" +
            // New amplitude presets have zero velocity and acceleration at both
            // ends. Rebounds peak at 60%, leaving 40% for a gentle C2 return.
            "function smoother(u){if(u<=0)return 0;if(u>=1)return 1;return u*u*u*(u*(6*u-15)+10);}\n" +
            "function rebound(u,a){if(u<=0)return 0;if(u>=1)return 1;return u<=0.6?(1+a)*smoother(u/0.6):1+a*(1-smoother((u-0.6)/0.4));}\n" +
            "if(!clips.length) return [1,1,0,0,0][output];\n" +
            "for(var j=0;j<clips.length;j++) if(t>=clips[j].begin && t<clips[j].end) active.push(clips[j]);\n" +
            "var a,b,ta,tb,w=0;\n" +
            "if(active.length){\n" +
            " b=active[active.length-1]; a=active.length>1 ? active[active.length-2] : b;\n" +
            " ta=clock(a,t); tb=clock(b,t);\n" +
            " if(a!==b){\n" +
            "  var start=Math.max(a.begin,b.begin), end=Math.min(a.end,b.end);\n" +
            "  if(b.end<a.end){var fade=Math.min(b.fade,(end-start)/2); w=100*Math.min(edge(t-start,fade),edge(end-t,fade));}\n" +
            "  else w=100*edge(t-start,end-start);\n" +
            "  if(b.curve===1) w=100*spring(w/100);\n" +
            "  else if(b.curve===2) w=100*overshoot(w/100);\n" +
            "  else if(b.curve===3) w=100*smoother(w/100);\n" +
            "  else if(b.curve===4 || b.curve===5) w=100*rebound(w/100,b.curve===4?0.03:0.075);\n" +
            " }\n" +
            "}else{\n" +
            " a=clips[0]; var found=false;\n" +
            " for(var n=0;n<clips.length;n++){var c=clips[n]; if(c.end<=t && (!found || c.end>=a.end)){a=c;found=true;}}\n" +
            " b=a; ta=tb=clock(a,found?a.end:a.begin);\n" +
            "}\n" +
            "return [a.slot,b.slot,ta,tb,w][output];\n" +
            "})()";
    }

    function expressionWeights(owner, output) {
        // Connected overlaps share a neutral envelope, with each outer edge's
        // stored fade. Interior crossfades never dip toward the neutral pose.
        return "// L2DAE_EXPRESSION_V2 owner=" + owner + "\n" +
            "(function(){\n" +
            "var owner=" + owner + ", output=" + output + ", clips=[], active=[], t=time;\n" +
            expressionScan(EXPRESSION_CHANNEL) +
            "for(var j=0;j<clips.length;j++) if(t>=clips[j].begin && t<clips[j].end) active.push(clips[j]);\n" +
            "if(!active.length) return [1,1,0,0][output];\n" +
            "var gb=clips[0].begin, ge=clips[0].end, fi=clips[0].fade, fo=clips[0].fade;\n" +
            "for(var n=1;n<clips.length;n++){var c=clips[n];\n" +
            " if(c.begin<ge){if(c.end>ge){ge=c.end;fo=c.fade;}}\n" +
            " else{if(t>=gb && t<ge) break; gb=c.begin; ge=c.end; fi=fo=c.fade;}\n" +
            "}\n" +
            "var strength=Math.min(edge(t-gb,Math.min(fi,(ge-gb)/2)),edge(ge-t,Math.min(fo,(ge-gb)/2)));\n" +
            "var b=active[active.length-1], a=active.length>1?active[active.length-2]:b, w=0;\n" +
            "if(a!==b){var start=Math.max(a.begin,b.begin), end=Math.min(a.end,b.end);\n" +
            " if(b.end<a.end){var f=Math.min(b.fade,(end-start)/2); w=Math.min(edge(t-start,f),edge(end-t,f));}\n" +
            " else w=edge(t-start,end-start);\n" +
            "}\n" +
            "return [a.slot,b.slot,100*strength*(1-w),100*strength*w][output];\n" +
            "})()";
    }

    function prepareClip(payload) {
        if (!payload || (typeof payload.kind !== "undefined" && payload.kind !== "motion" && payload.kind !== "expression")) {
            fail(text("Invalid timeline clip type. Choose a motion or expression again.", "时间线片段类型无效，请重新选择动作或表情。"));
        }
        var isExpression = payload.kind === "expression";
        var channel = isExpression ? EXPRESSION_CHANNEL : MOTION_CHANNEL;
        var names = channel.controls;
        var duration = isExpression && typeof payload.duration === "undefined" ? 3 : payload.duration;
        var append = isExpression && typeof payload.append === "undefined" ? false : payload.append;
        var transitionFrames = typeof payload.transitionFrames === "undefined" ? 30 : payload.transitionFrames;
        var transitionCurve = typeof payload.transitionCurve === "undefined" ? (isExpression ? 0 : 5) : payload.transitionCurve;
        if (!integer(payload.binding) || payload.binding < 1 || payload.binding > 2147483647 ||
            !integer(payload.slot) || payload.slot < 1 || payload.slot > 16777216 ||
            !finite(duration) || duration <= 0 || duration > 86400 ||
            typeof payload.label !== "string" || typeof append !== "boolean" ||
            !integer(transitionFrames) || transitionFrames < 0 || transitionFrames > 100000 ||
            !integer(transitionCurve) || transitionCurve < 0 || transitionCurve > 5) {
            fail(text("Invalid timeline clip settings. Check the motion duration and transition.", "时间线片段设置无效，请检查动作时长和过渡设置。"));
        }
        if (!app.project) fail(text("Open an After Effects project before importing a motion or expression.", "请先打开一个 AE 工程，再导入动作或表情。"));
        var matches = [], i, j, k;
        for (i = 1; i <= app.project.numItems; ++i) {
            var item = app.project.item(i);
            if (!(item instanceof CompItem)) continue;
            for (j = 1; j <= item.numLayers; ++j) {
                var layer = item.layer(j), effects = layer.property("ADBE Effect Parade");
                if (!effects) continue;
                for (k = 1; k <= effects.numProperties; ++k) {
                    var effect = effects.property(k), binding;
                    if (effect.matchName === MATCH) {
                        binding = findParameter(effect, channel.binding);
                        if (binding && Number(binding.value) === payload.binding) {
                            matches.push({ comp: item, layer: layer, effect: effect });
                        }
                    }
                }
            }
        }
        if (matches.length !== 1) fail(matches.length ?
            text("The model effect was duplicated before the clip was created. Import again on the target model.", "创建片段前检测到模型效果被复制，请在目标模型上重新导入。") :
            text("The target model effect was not found. Import again on the target model.", "找不到目标模型效果，请在目标模型上重新导入。"));
        var target = matches[0], comp = target.comp, fx = target.effect;
        if (target.layer.locked) fail(text("Unlock the model layer before importing a motion or expression.", "请先解锁模型图层，再导入动作或表情。"));
        var controls = [], legacyControls = [], oldOwner = 0, expressionsOwned = 0;
        for (i = 0; i < names.length; ++i) {
            var control = property(fx, names[i]), oldExpression = control.expression || "";
            var foundOwner = ownerFromExpression(oldExpression, channel);
            if (i < channel.legacy.length) {
                var legacyControl = property(fx, channel.legacy[i]);
                var legacyExpression = legacyControl.expression || "";
                var legacyOwner = ownerFromExpression(legacyExpression, channel);
                if (legacyControl.numKeys || (legacyExpression && !legacyOwner)) {
                    fail(text("Existing keyframes or custom expressions would be overwritten. Add a new AeGO Flash effect before importing clips.", "现有关键帧或自定义表达式会被覆盖，请使用新的 AeGO Flash 效果导入时间线片段。"));
                }
                if (foundOwner && legacyOwner && foundOwner !== legacyOwner)
                    fail(text("Timeline expressions belong to a different clip group. Restore the original expressions or use a new AeGO Flash effect.", "时间线表达式关联了不同的片段组，请恢复原表达式或使用新的 AeGO Flash 效果。"));
                if (!foundOwner && !oldExpression && legacyOwner) foundOwner = legacyOwner;
                if (legacyOwner) legacyControls.push({ property: legacyControl, expression: legacyExpression,
                    enabled: legacyControl.expressionEnabled, value: legacyControl.value });
            }
            if (control.numKeys || (oldExpression && !foundOwner)) {
                fail(text("Existing keyframes or custom expressions would be overwritten. Add a new AeGO Flash effect before importing clips.", "现有关键帧或自定义表达式会被覆盖，请使用新的 AeGO Flash 效果导入时间线片段。"));
            }
            if (foundOwner) {
                if (oldOwner && oldOwner !== foundOwner) fail(text("Timeline expressions belong to a different clip group. Restore the original expressions or use a new AeGO Flash effect.", "时间线表达式关联了不同的片段组，请恢复原表达式或使用新的 AeGO Flash 效果。"));
                oldOwner = foundOwner;
                ++expressionsOwned;
            }
            if (!control.canSetExpression) fail(text("This effect cannot use timeline expressions. Use a new AeGO Flash effect.", "此效果无法使用时间线表达式，请使用新的 AeGO Flash 效果。"));
            controls.push({ property: control, expression: oldExpression,
                            enabled: control.expressionEnabled, value: control.value });
        }
        if (expressionsOwned !== 0 && expressionsOwned !== names.length) {
            fail(text("Some timeline expressions were removed. Restore them or use a new AeGO Flash effect.", "部分时间线表达式已被移除，请恢复原表达式或使用新的 AeGO Flash 效果。"));
        }
        if (!oldOwner && integer(payload.previousBinding) && payload.previousBinding > 0) oldOwner = payload.previousBinding;
        var manual = null, loop = null, oldManual = 0, oldLoop = 0;
        if (!isExpression) {
            manual = property(fx, "Keyframe motion time");
            if (manual.numKeys || manual.expression) fail(text("Remove keyframes or expressions on Motion Time Keys before using clips.", "使用片段前，请移除“动作时间关键帧”选项上的关键帧或表达式。"));
            loop = property(fx, "Loop motion");
            if (loop.numKeys || loop.expression) fail(text("Remove keyframes or expressions on Loop Motion before using clips.", "使用片段前，请移除“循环动作”选项上的关键帧或表达式。"));
            oldManual = manual.value;
            oldLoop = loop.value;
        }
        var oldClips = findClips(comp, oldOwner, channel);
        for (i = 0; i < oldClips.length; ++i) {
            if (oldClips[i].layer.locked) fail(text("Unlock the existing AeGO Flash " + (isExpression ? "expression" : "motion") + " clip before importing another.", "请先解锁现有的 AeGO Flash " + (isExpression ? "表情" : "动作") + "片段，再导入新片段。"));
        }
        var cloneOld = ownedByAnotherEffect(comp, target, oldOwner, channel);
        var fade = transitionFrames * comp.frameDuration;
        var start = comp.time, latest = null;
        if (append && oldClips.length) {
            for (i = 0; i < oldClips.length; ++i) {
                if (!latest || oldClips[i].end > latest.end) latest = oldClips[i];
            }
            start = latest.end - Math.min(fade, duration / 2, (latest.end - latest.begin) / 2);
        }
        return { payload: payload, channel: channel, isExpression: isExpression,
                 target: target, comp: comp, controls: controls, legacyControls: legacyControls, oldClips: oldClips,
                 cloneOld: cloneOld, manual: manual, loop: loop, oldManual: oldManual,
                 oldLoop: oldLoop, start: start, duration: duration, fade: fade, curve: transitionCurve };
    }
    function addClips(payloads) {
        if (!payloads || typeof payloads.length !== "number" || payloads.length < 1 || payloads.length > 2)
            fail(text("Choose one motion, one expression, or both.", "请选择一个动作、一个表情，或同时选择动作与表情。"));
        var plans = [], i, j, plan, motion = null, face = null;
        // Resolve every target and protect every edited control before mutation.
        for (i = 0; i < payloads.length; ++i) {
            plan = prepareClip(payloads[i]);
            if (plan.isExpression) {
                if (face) fail(text("Choose only one expression for this import.", "每次导入只能选择一个表情。"));
                face = plan;
            } else {
                if (motion) fail(text("Choose only one motion for this import.", "每次导入只能选择一个动作。"));
                motion = plan;
            }
            plans.push(plan);
        }
        var comp = plans[0].comp;
        if (motion && face) {
            if (motion.comp !== face.comp || motion.target.layer.index !== face.target.layer.index ||
                motion.target.effect.propertyIndex !== face.target.effect.propertyIndex)
                fail(text("A motion and expression imported together must belong to the same model effect.", "同时导入的动作与表情必须属于同一个模型效果。"));
            face.start = motion.start;
        }
        for (i = 0; i < plans.length; ++i) {
            plan = plans[i]; plan.end = Math.min(comp.duration, plan.start + plan.duration);
            if (!finite(plan.start) || plan.start < 0 || plan.start >= comp.duration ||
                plan.end - plan.start < comp.frameDuration) {
                fail(text("Not enough time remains at the current position. Extend the composition or start earlier.", "当前位置剩余时长不足，请延长合成或选择更早的时间后再导入。"));
            }
        }
        var selection = [], existingItems = {};
        for (i = 1; i <= comp.numLayers; ++i) if (comp.layer(i).selected) selection.push(comp.layer(i));
        for (i = 1; i <= app.project.numItems; ++i) existingItems["item" + app.project.item(i).id] = true;
        var created = [], createdSources = [], retagged = [], sourceCandidates = [], renamedSources = [];
        var startedUndo = false, failure = null;
        try {
            app.beginUndoGroup(text("Import AeGO Flash Clips", "导入 AeGO Flash 动作与表情"));
            startedUndo = true;
            for (j = 0; j < plans.length; ++j) {
                plan = plans[j];
                var payload = plan.payload, channel = plan.channel;
                for (i = 0; i < plan.oldClips.length; ++i) {
                    var info = plan.oldClips[i], clip = info.layer;
                    if (plan.cloneOld) { clip = clip.duplicate(); created.push(clip); }
                    var marker = clip.property("ADBE Marker");
                    var value = marker.keyValue(info.key);
                    if (!plan.cloneOld) retagged.push({ marker: marker, key: info.key,
                        comment: value.comment, parameters: value.getParameters(), layer: clip, name: clip.name });
                    // A migrated old marker gets its visible name from the layer;
                    // current readable labels and unrelated parameters are retained.
                    var legacyMarker = String(value.comment).indexOf(channel.prefix) === 0;
                    if (legacyMarker) clip.name = clipName(clip.name);
                    var label = legacyMarker ? clip.name : value.comment;
                    setMetadata(value, channel, payload.binding, info.slot, info.duration, info.fade, label, info.curve);
                    marker.setValueAtKey(info.key, value);
                    sourceCandidates.push({ layer: clip, comp: comp, name: clipName(label), migrate: true });
                }
                var newClip = comp.layers.addNull(Math.max(plan.duration, comp.frameDuration));
                created.push(newClip);
                var newSource = newClip.source;
                if (newSource && typeof newSource.id === "number" && !existingItems["item" + newSource.id]) {
                    createdSources.push(newSource);
                }
                newClip.name = clipName(payload.label);
                sourceCandidates.push({ layer: newClip, comp: comp, name: newClip.name, migrate: false });
                newClip.guideLayer = true; newClip.enabled = true;
                newClip.label = plan.isExpression ? 13 : 9;
                newClip.startTime = plan.start; newClip.inPoint = plan.start; newClip.outPoint = plan.end;
                var metadata = new MarkerValue(newClip.name);
                setMetadata(metadata, channel, payload.binding, payload.slot, plan.duration, plan.fade, payload.label, plan.curve);
                newClip.property("ADBE Marker").setValueAtTime(plan.start, metadata);
                if (plan.manual) plan.manual.setValue(1);
                if (plan.loop) plan.loop.setValue(0);
                for (i = 0; i < plan.legacyControls.length; ++i) {
                    plan.legacyControls[i].property.expression = "";
                }
                for (i = 0; i < plan.controls.length; ++i) {
                    var control = plan.controls[i].property;
                    control.expression = plan.isExpression ? expressionWeights(payload.binding, i) : expression(payload.binding, i);
                    control.expressionEnabled = true;
                    if (control.expressionError) fail(text("After Effects could not evaluate a timeline expression: ", "AE 无法执行时间线表达式：") + control.expressionError);
                }
            }
            // AE normally displays Source Name in the Timeline. Name our owned
            // footage too, so both column modes show the imported asset name.
            renameClipSources(sourceCandidates, existingItems, renamedSources);
        } catch (error) {
            failure = error;
            // Restore the entire batch using snapshots, never global Undo (which
            // might undo a native model import or the user's unrelated edit).
            for (j = plans.length - 1; j >= 0; --j) {
                plan = plans[j];
                var restoreControls = plan.controls.concat(plan.legacyControls);
                for (i = restoreControls.length - 1; i >= 0; --i) {
                    try {
                        var old = restoreControls[i];
                        old.property.expression = old.expression;
                        old.property.expressionEnabled = old.enabled;
                        if (!old.expression) old.property.setValue(old.value);
                    } catch (ignoredControl) {}
                }
                try { if (plan.manual) plan.manual.setValue(plan.oldManual); } catch (ignoredManual) {}
                try { if (plan.loop) plan.loop.setValue(plan.oldLoop); } catch (ignoredLoop) {}
            }
            for (i = retagged.length - 1; i >= 0; --i) {
                try {
                    var oldTag = retagged[i], restored = oldTag.marker.keyValue(oldTag.key);
                    oldTag.layer.name = oldTag.name;
                    restored.comment = oldTag.comment; restored.setParameters(oldTag.parameters);
                    oldTag.marker.setValueAtKey(oldTag.key, restored);
                } catch (ignoredMarker) {}
            }
            for (i = renamedSources.length - 1; i >= 0; --i) {
                try { renamedSources[i].source.name = renamedSources[i].name; } catch (ignoredSourceName) {}
            }
            for (i = created.length - 1; i >= 0; --i) {
                try { created[i].remove(); } catch (ignoredLayer) {}
            }
            for (i = createdSources.length - 1; i >= 0; --i) {
                try { if (createdSources[i].usedIn.length === 0) createdSources[i].remove(); } catch (ignoredSource) {}
            }
        } finally {
            // Both imports share one undo group and preserve the prior selection.
            if (startedUndo) {
                for (i = 1; i <= comp.numLayers; ++i) {
                    try { comp.layer(i).selected = false; } catch (ignoredDeselect) {}
                }
                for (i = 0; i < selection.length; ++i) {
                    try { selection[i].selected = true; } catch (ignoredSelect) {}
                }
                app.endUndoGroup();
            }
        }
        if (failure) throw failure;
        return "OK";
    }
    global.L2DAE_addClips = addClips;
    global.L2DAE_addClip = function (payload) { return addClips([payload]); };
})(this);
